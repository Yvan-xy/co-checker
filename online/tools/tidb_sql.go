// tidb_sql is a deliberately small SQL control utility for the online TPC-C
// and YCSB workflows. It is run from the TiDB module so it reuses TiDB's
// pinned MySQL driver instead of adding another dependency to co-checker.
package main

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

const tpccTables = "customer,district,history,item,new_order,order_line,orders,stock,warehouse"

const (
	ycsbTableName       = "usertable"
	ycsbKeyLength       = 64
	ycsbDefaultFields   = 10
	ycsbDefaultFieldLen = 100
	ycsbDefaultPageSize = 4096
	ycsbRetryLimit      = "100"
)

var identifierPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

var expectedCheckedSessionVariables = map[string]string{
	"transaction_isolation":             "REPEATABLE-READ",
	"tidb_txn_mode":                     "OPTIMISTIC",
	"tidb_enable_async_commit":          "0",
	"tidb_enable_1pc":                   "0",
	"tidb_low_resolution_tso":           "0",
	"tidb_read_staleness":               "0",
	"tidb_snapshot":                     "",
	"tidb_store_batch_size":             "0",
	"tidb_allow_batch_cop":              "0",
	"tidb_index_lookup_pushdown_policy": "FORCE",
}

type options struct {
	action         string
	address        string
	user           string
	password       string
	database       string
	params         string
	tables         string
	statement      string
	timeout        time.Duration
	fieldCount     int
	fieldLength    int
	digestPageSize int
}

type planResult struct {
	Name      string     `json:"name"`
	Operators []string   `json:"operators"`
	Rows      [][]string `json:"explain_rows"`
}

type gateReport struct {
	SessionVariables map[string]string `json:"session_variables"`
	Plans            []planResult      `json:"plans"`
	Outcome          string            `json:"outcome"`
}

func main() {
	var cfg options
	flag.StringVar(&cfg.action, "action", "ping", "ping, exec, assert-empty, tpcc-digest, tpcc-gate, ycsb-schema, assert-ycsb-empty, ycsb-digest, or ycsb-gate")
	flag.StringVar(&cfg.address, "address", "127.0.0.1:4000", "TiDB SQL address")
	flag.StringVar(&cfg.user, "user", "root", "SQL user")
	flag.StringVar(&cfg.password, "password", "", "SQL password")
	flag.StringVar(&cfg.database, "database", "tpcc", "database name")
	flag.StringVar(&cfg.params, "params", "", "additional MySQL DSN parameters")
	flag.StringVar(&cfg.tables, "tables", tpccTables, "comma-separated table names")
	flag.StringVar(&cfg.statement, "statement", "", "SQL statement for the exec action")
	flag.DurationVar(&cfg.timeout, "timeout", 30*time.Second, "operation timeout")
	flag.IntVar(&cfg.fieldCount, "field-count", ycsbDefaultFields, "YCSB FIELD column count")
	flag.IntVar(&cfg.fieldLength, "field-length", ycsbDefaultFieldLen, "YCSB FIELD VARCHAR length")
	flag.IntVar(&cfg.digestPageSize, "digest-page-size", ycsbDefaultPageSize, "maximum YCSB rows per digest transaction")
	flag.Parse()

	if err := run(cfg); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(cfg options) error {
	if cfg.address == "" || cfg.user == "" || cfg.timeout <= 0 {
		return errors.New("address, user, and a positive timeout are required")
	}
	if cfg.database != "" && !identifierPattern.MatchString(cfg.database) {
		return fmt.Errorf("invalid database identifier %q", cfg.database)
	}

	selectedDatabase := cfg.database
	if cfg.action == "ycsb-schema" {
		selectedDatabase = ""
	}
	dsn := fmt.Sprintf("%s:%s@tcp(%s)/%s?parseTime=true&multiStatements=true&tls=false",
		cfg.user, cfg.password, cfg.address, selectedDatabase)
	if strings.TrimSpace(cfg.params) != "" {
		dsn += "&" + strings.TrimPrefix(strings.TrimSpace(cfg.params), "&")
	}
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		return fmt.Errorf("open TiDB connection: %w", err)
	}
	defer db.Close()

	ctx, cancel := context.WithTimeout(context.Background(), cfg.timeout)
	defer cancel()
	connection, err := db.Conn(ctx)
	if err != nil {
		return fmt.Errorf("acquire TiDB connection: %w", err)
	}
	defer connection.Close()

	switch cfg.action {
	case "ping":
		if err := connection.PingContext(ctx); err != nil {
			return fmt.Errorf("ping TiDB: %w", err)
		}
		return nil
	case "exec":
		if strings.TrimSpace(cfg.statement) == "" {
			return errors.New("exec requires a nonempty statement")
		}
		if _, err := connection.ExecContext(ctx, cfg.statement); err != nil {
			return fmt.Errorf("execute SQL: %w", err)
		}
		return nil
	case "assert-empty":
		return assertEmpty(ctx, connection, cfg.tables)
	case "tpcc-digest":
		return tpccDigest(ctx, connection)
	case "new-order-gate", "tpcc-gate":
		return tpccGate(ctx, connection)
	case "ycsb-schema":
		return ycsbSchema(ctx, connection, cfg.database, cfg.fieldCount, cfg.fieldLength)
	case "assert-ycsb-empty":
		return assertYCSBEmpty(ctx, connection)
	case "ycsb-digest":
		return ycsbDigest(ctx, connection, cfg.fieldCount, cfg.digestPageSize)
	case "ycsb-gate":
		return ycsbGate(ctx, connection, cfg.database, cfg.fieldCount, cfg.fieldLength)
	default:
		return fmt.Errorf("unknown action %q", cfg.action)
	}
}

func parseTables(text string) ([]string, error) {
	parts := strings.Split(text, ",")
	tables := make([]string, 0, len(parts))
	seen := make(map[string]struct{}, len(parts))
	for _, part := range parts {
		table := strings.TrimSpace(part)
		if !identifierPattern.MatchString(table) {
			return nil, fmt.Errorf("invalid table identifier %q", table)
		}
		if _, duplicate := seen[table]; duplicate {
			return nil, fmt.Errorf("duplicate table %q", table)
		}
		seen[table] = struct{}{}
		tables = append(tables, table)
	}
	if len(tables) == 0 {
		return nil, errors.New("at least one table is required")
	}
	sort.Strings(tables)
	return tables, nil
}

func assertEmpty(ctx context.Context, connection *sql.Conn, tableText string) error {
	tables, err := parseTables(tableText)
	if err != nil {
		return err
	}
	counts := make(map[string]int64, len(tables))
	for _, table := range tables {
		var count int64
		query := "SELECT COUNT(*) FROM `" + table + "`"
		if err := connection.QueryRowContext(ctx, query).Scan(&count); err != nil {
			return fmt.Errorf("count %s: %w", table, err)
		}
		counts[table] = count
		if count != 0 {
			encoded, _ := json.Marshal(counts)
			return fmt.Errorf("TPC-C template is not empty: %s", encoded)
		}
	}
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(counts)
}

type ycsbSchemaReport struct {
	FormatVersion int    `json:"format_version"`
	Database      string `json:"database"`
	Table         string `json:"table"`
	FieldCount    int    `json:"field_count"`
	FieldLength   int    `json:"field_length"`
	Outcome       string `json:"outcome"`
}

func validateYCSBShapeParameters(fieldCount, fieldLength int) error {
	if fieldCount < 1 || fieldCount > 1024 {
		return fmt.Errorf("YCSB field count %d is outside [1,1024]", fieldCount)
	}
	if fieldLength < 1 || fieldLength > 65535 {
		return fmt.Errorf("YCSB field length %d is outside [1,65535]", fieldLength)
	}
	return nil
}

func ycsbColumnNames(fieldCount int) []string {
	columns := make([]string, fieldCount)
	for index := range columns {
		columns[index] = fmt.Sprintf("FIELD%d", index)
	}
	return columns
}

func ycsbCreateTableSQL(database string, fieldCount, fieldLength int) (string, error) {
	if !identifierPattern.MatchString(database) {
		return "", fmt.Errorf("invalid database identifier %q", database)
	}
	if err := validateYCSBShapeParameters(fieldCount, fieldLength); err != nil {
		return "", err
	}
	var builder strings.Builder
	fmt.Fprintf(&builder, "CREATE TABLE IF NOT EXISTS `%s`.`%s` (YCSB_KEY VARCHAR(%d) PRIMARY KEY /*T![clustered_index] CLUSTERED */", database, ycsbTableName, ycsbKeyLength)
	for index := 0; index < fieldCount; index++ {
		fmt.Fprintf(&builder, ", FIELD%d VARCHAR(%d)", index, fieldLength)
	}
	builder.WriteString(");")
	return builder.String(), nil
}

func ycsbSchema(ctx context.Context, connection *sql.Conn, database string, fieldCount, fieldLength int) error {
	createTable, err := ycsbCreateTableSQL(database, fieldCount, fieldLength)
	if err != nil {
		return err
	}
	if _, err := connection.ExecContext(ctx, "CREATE DATABASE IF NOT EXISTS `"+database+"`"); err != nil {
		return fmt.Errorf("create YCSB database: %w", err)
	}
	if _, err := connection.ExecContext(ctx, createTable); err != nil {
		return fmt.Errorf("create YCSB table: %w", err)
	}
	report := ycsbSchemaReport{
		FormatVersion: 1,
		Database:      database,
		Table:         ycsbTableName,
		FieldCount:    fieldCount,
		FieldLength:   fieldLength,
		Outcome:       "pass",
	}
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(report)
}

type ycsbEmptyReport struct {
	FormatVersion int    `json:"format_version"`
	Table         string `json:"table"`
	RowCount      uint64 `json:"row_count"`
	Outcome       string `json:"outcome"`
}

func assertYCSBEmpty(ctx context.Context, connection *sql.Conn) error {
	var count uint64
	if err := connection.QueryRowContext(ctx, "SELECT COUNT(*) FROM `"+ycsbTableName+"`").Scan(&count); err != nil {
		return fmt.Errorf("count YCSB table: %w", err)
	}
	if count != 0 {
		return fmt.Errorf("YCSB template is not empty: %d row(s)", count)
	}
	report := ycsbEmptyReport{FormatVersion: 1, Table: ycsbTableName, RowCount: count, Outcome: "pass"}
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(report)
}

type ycsbDigestCell struct {
	present bool
	value   []byte
}

type ycsbDigestRow struct {
	key    []byte
	fields []ycsbDigestCell
}

type ycsbDigestReport struct {
	FormatVersion       int    `json:"format_version"`
	Algorithm           string `json:"algorithm"`
	Encoding            string `json:"canonical_encoding"`
	Digest              string `json:"database_digest_sha256"`
	SchemaDigest        string `json:"schema_digest_sha256"`
	DataDigest          string `json:"data_digest_sha256"`
	TotalRows           uint64 `json:"total_rows"`
	PageCount           uint64 `json:"page_count"`
	ConfiguredPageSize  int    `json:"configured_page_size"`
	MaximumObservedPage int    `json:"maximum_observed_page_size"`
}

type ycsbPageFetcher func(afterKey string, limit int) ([]ycsbDigestRow, error)

func buildYCSBDigest(createStatement string, fieldCount, pageSize int, fetch ycsbPageFetcher) (ycsbDigestReport, error) {
	report := ycsbDigestReport{
		FormatVersion:      1,
		Algorithm:          "sha256",
		Encoding:           "schema frames table name and exact SHOW CREATE TABLE text; data frames ordered column names and keyset-ordered rows with uint64-le lengths plus one-byte NULL/present field markers",
		ConfiguredPageSize: pageSize,
	}
	if strings.TrimSpace(createStatement) == "" {
		return report, errors.New("YCSB digest requires a nonempty CREATE TABLE statement")
	}
	if err := validateYCSBShapeParameters(fieldCount, 1); err != nil {
		return report, err
	}
	if pageSize <= 0 {
		return report, fmt.Errorf("YCSB digest page size must be positive, got %d", pageSize)
	}
	if fetch == nil {
		return report, errors.New("YCSB digest requires a page fetcher")
	}

	schemaDigest := sha256.New()
	writeDigestBytes(schemaDigest, []byte(ycsbTableName))
	writeDigestBytes(schemaDigest, []byte(createStatement))

	dataDigest := sha256.New()
	writeDigestBytes(dataDigest, []byte(ycsbTableName))
	columns := append([]string{"YCSB_KEY"}, ycsbColumnNames(fieldCount)...)
	var frame [8]byte
	binary.LittleEndian.PutUint64(frame[:], uint64(len(columns)))
	_, _ = dataDigest.Write(frame[:])
	for _, column := range columns {
		writeDigestBytes(dataDigest, []byte(column))
	}

	afterKey := ""
	for {
		page, err := fetch(afterKey, pageSize)
		if err != nil {
			return report, err
		}
		report.PageCount++
		if len(page) > pageSize {
			return report, fmt.Errorf("YCSB digest page returned %d rows, limit %d", len(page), pageSize)
		}
		if len(page) > report.MaximumObservedPage {
			report.MaximumObservedPage = len(page)
		}
		previousKey := afterKey
		for _, row := range page {
			key := string(row.key)
			if key <= previousKey {
				return report, fmt.Errorf("YCSB digest keys are not strictly increasing: %q after %q", key, previousKey)
			}
			if len(row.fields) != fieldCount {
				return report, fmt.Errorf("YCSB digest row %q has %d fields, want %d", key, len(row.fields), fieldCount)
			}
			writeDigestBytes(dataDigest, row.key)
			for _, field := range row.fields {
				if !field.present {
					_, _ = dataDigest.Write([]byte{0})
					continue
				}
				_, _ = dataDigest.Write([]byte{1})
				writeDigestBytes(dataDigest, field.value)
			}
			previousKey = key
			report.TotalRows++
		}
		if len(page) != 0 {
			afterKey = string(page[len(page)-1].key)
		}
		if len(page) < pageSize {
			break
		}
	}
	binary.LittleEndian.PutUint64(frame[:], report.TotalRows)
	_, _ = dataDigest.Write(frame[:])

	schemaSum := schemaDigest.Sum(nil)
	dataSum := dataDigest.Sum(nil)
	combined := sha256.New()
	writeDigestBytes(combined, schemaSum)
	writeDigestBytes(combined, dataSum)
	report.SchemaDigest = hex.EncodeToString(schemaSum)
	report.DataDigest = hex.EncodeToString(dataSum)
	report.Digest = hex.EncodeToString(combined.Sum(nil))
	return report, nil
}

func fetchYCSBDigestPage(ctx context.Context, connection *sql.Conn, columns []string, afterKey string, limit int) ([]ycsbDigestRow, error) {
	// TiDB rejects SQL READ ONLY transaction syntax unless its no-op-functions
	// compatibility switch is enabled. Keep the controlled DSN exact and make
	// this transaction read-only by construction: it executes only the bounded
	// SELECT below before commit.
	tx, err := connection.BeginTx(ctx, nil)
	if err != nil {
		return nil, fmt.Errorf("begin query-only YCSB digest page after %q: %w", afterKey, err)
	}
	committed := false
	defer func() {
		if !committed {
			_ = tx.Rollback()
		}
	}()
	query := "SELECT YCSB_KEY," + strings.Join(columns, ",") + " FROM `" + ycsbTableName + "` WHERE YCSB_KEY > ? ORDER BY YCSB_KEY LIMIT ?"
	rows, err := tx.QueryContext(ctx, query, afterKey, limit)
	if err != nil {
		return nil, fmt.Errorf("query YCSB digest page after %q: %w", afterKey, err)
	}
	page := make([]ycsbDigestRow, 0, limit)
	for rows.Next() {
		var key sql.RawBytes
		fields := make([]sql.RawBytes, len(columns))
		destinations := make([]any, 0, len(columns)+1)
		destinations = append(destinations, &key)
		for index := range fields {
			destinations = append(destinations, &fields[index])
		}
		if err := rows.Scan(destinations...); err != nil {
			rows.Close()
			return nil, fmt.Errorf("scan YCSB digest row: %w", err)
		}
		row := ycsbDigestRow{key: append([]byte(nil), key...), fields: make([]ycsbDigestCell, len(fields))}
		for index, field := range fields {
			if field != nil {
				row.fields[index] = ycsbDigestCell{present: true, value: append([]byte(nil), field...)}
			}
		}
		page = append(page, row)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, fmt.Errorf("scan YCSB digest page: %w", err)
	}
	if err := rows.Close(); err != nil {
		return nil, fmt.Errorf("close YCSB digest page: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return nil, fmt.Errorf("commit YCSB digest page: %w", err)
	}
	committed = true
	return page, nil
}

func ycsbDigest(ctx context.Context, connection *sql.Conn, fieldCount, pageSize int) error {
	if err := validateYCSBShapeParameters(fieldCount, 1); err != nil {
		return err
	}
	if pageSize <= 0 {
		return fmt.Errorf("YCSB digest page size must be positive, got %d", pageSize)
	}
	var shownName, createStatement string
	if err := connection.QueryRowContext(ctx, "SHOW CREATE TABLE `"+ycsbTableName+"`").Scan(&shownName, &createStatement); err != nil {
		return fmt.Errorf("digest YCSB schema: %w", err)
	}
	if !strings.EqualFold(shownName, ycsbTableName) {
		return fmt.Errorf("digest YCSB schema: SHOW CREATE returned table %q", shownName)
	}
	columns := ycsbColumnNames(fieldCount)
	report, err := buildYCSBDigest(createStatement, fieldCount, pageSize, func(afterKey string, limit int) ([]ycsbDigestRow, error) {
		return fetchYCSBDigestPage(ctx, connection, columns, afterKey, limit)
	})
	if err != nil {
		return err
	}
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(report)
}

type tpccDigestTable struct {
	name    string
	orderBy string
}

var tpccDigestTables = []tpccDigestTable{
	{name: "customer", orderBy: "c_w_id,c_d_id,c_id"},
	{name: "district", orderBy: "d_w_id,d_id"},
	{name: "history", orderBy: "h_c_w_id,h_c_d_id,h_c_id,h_w_id,h_d_id,h_date,h_amount,h_data"},
	{name: "item", orderBy: "i_id"},
	{name: "new_order", orderBy: "no_w_id,no_d_id,no_o_id"},
	{name: "order_line", orderBy: "ol_w_id,ol_d_id,ol_o_id,ol_number"},
	{name: "orders", orderBy: "o_w_id,o_d_id,o_id"},
	{name: "stock", orderBy: "s_w_id,s_i_id"},
	{name: "warehouse", orderBy: "w_id"},
}

type tpccDigestReport struct {
	FormatVersion  int               `json:"format_version"`
	Algorithm      string            `json:"algorithm"`
	Encoding       string            `json:"canonical_encoding"`
	Digest         string            `json:"database_digest_sha256"`
	SchemaDigest   string            `json:"schema_digest_sha256"`
	DataDigest     string            `json:"data_digest_sha256"`
	TotalRows      uint64            `json:"total_rows"`
	TableRowCounts map[string]uint64 `json:"table_row_counts"`
}

type digestWriter interface {
	Write([]byte) (int, error)
}

func writeDigestBytes(writer digestWriter, value []byte) {
	var length [8]byte
	binary.LittleEndian.PutUint64(length[:], uint64(len(value)))
	_, _ = writer.Write(length[:])
	_, _ = writer.Write(value)
}

func tpccDigest(ctx context.Context, connection *sql.Conn) error {
	schemaDigest := sha256.New()
	dataDigest := sha256.New()
	report := tpccDigestReport{
		FormatVersion: 2,
		Algorithm:     "sha256",
		Encoding: "database digest frames schema then data SHA256 bytes; schema frames ordered table names and exact SHOW CREATE TABLE text; " +
			"data frames table/column names and ordered rows with uint64-le lengths, one-byte NULL/present cell markers, and MySQL protocol bytes",
		TableRowCounts: make(map[string]uint64, len(tpccDigestTables)),
	}
	for _, table := range tpccDigestTables {
		writeDigestBytes(schemaDigest, []byte(table.name))
		var shownName, createStatement string
		if err := connection.QueryRowContext(ctx, "SHOW CREATE TABLE `"+table.name+"`").Scan(&shownName, &createStatement); err != nil {
			return fmt.Errorf("digest schema %s: %w", table.name, err)
		}
		if !strings.EqualFold(shownName, table.name) {
			return fmt.Errorf("digest schema %s: SHOW CREATE returned table %q", table.name, shownName)
		}
		writeDigestBytes(schemaDigest, []byte(createStatement))

		writeDigestBytes(dataDigest, []byte(table.name))
		rows, err := connection.QueryContext(ctx, "SELECT * FROM `"+table.name+"` ORDER BY "+table.orderBy)
		if err != nil {
			return fmt.Errorf("digest query %s: %w", table.name, err)
		}
		columns, err := rows.Columns()
		if err != nil {
			rows.Close()
			return fmt.Errorf("digest columns %s: %w", table.name, err)
		}
		var frame [8]byte
		binary.LittleEndian.PutUint64(frame[:], uint64(len(columns)))
		_, _ = dataDigest.Write(frame[:])
		for _, column := range columns {
			writeDigestBytes(dataDigest, []byte(column))
		}
		var rowCount uint64
		for rows.Next() {
			values := make([]sql.RawBytes, len(columns))
			destinations := make([]any, len(values))
			for i := range values {
				destinations[i] = &values[i]
			}
			if err := rows.Scan(destinations...); err != nil {
				rows.Close()
				return fmt.Errorf("digest row %s: %w", table.name, err)
			}
			for _, value := range values {
				if value == nil {
					_, _ = dataDigest.Write([]byte{0})
					continue
				}
				_, _ = dataDigest.Write([]byte{1})
				writeDigestBytes(dataDigest, value)
			}
			rowCount++
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return fmt.Errorf("digest scan %s: %w", table.name, err)
		}
		if err := rows.Close(); err != nil {
			return fmt.Errorf("close digest rows %s: %w", table.name, err)
		}
		binary.LittleEndian.PutUint64(frame[:], rowCount)
		_, _ = dataDigest.Write(frame[:])
		report.TableRowCounts[table.name] = rowCount
		report.TotalRows += rowCount
	}

	schemaSum := schemaDigest.Sum(nil)
	dataSum := dataDigest.Sum(nil)
	databaseDigest := sha256.New()
	writeDigestBytes(databaseDigest, schemaSum)
	writeDigestBytes(databaseDigest, dataSum)
	report.SchemaDigest = hex.EncodeToString(schemaSum)
	report.DataDigest = hex.EncodeToString(dataSum)
	report.Digest = hex.EncodeToString(databaseDigest.Sum(nil))
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(report)
}

type ycsbColumnShape struct {
	name       string
	columnType string
	nullable   string
	columnKey  string
}

type ycsbIndexShape struct {
	name      string
	column    string
	sequence  int
	nonUnique int
}

type ycsbTableShape struct {
	tableType     string
	createOptions string
	createSQL     string
	partitioned   bool
	columns       []ycsbColumnShape
	indexes       []ycsbIndexShape
}

func validateYCSBTableShape(shape ycsbTableShape, fieldCount, fieldLength int) error {
	if err := validateYCSBShapeParameters(fieldCount, fieldLength); err != nil {
		return err
	}
	if !strings.EqualFold(shape.tableType, "BASE TABLE") {
		return fmt.Errorf("YCSB table type is %q, want BASE TABLE", shape.tableType)
	}
	if strings.TrimSpace(shape.createOptions) != "" {
		return fmt.Errorf("YCSB table has unexpected create options %q", shape.createOptions)
	}
	if shape.partitioned {
		return errors.New("YCSB table must not be partitioned")
	}
	createLower := strings.ToLower(shape.createSQL)
	if !strings.Contains(createLower, "clustered") {
		return errors.New("YCSB primary key is not declared CLUSTERED")
	}
	for _, unsupported := range []string{" temporary", " cached", " partition by"} {
		if strings.Contains(createLower, unsupported) {
			return fmt.Errorf("YCSB table uses unsupported schema feature %q", strings.TrimSpace(unsupported))
		}
	}
	if len(shape.columns) != fieldCount+1 {
		return fmt.Errorf("YCSB table has %d columns, want %d", len(shape.columns), fieldCount+1)
	}
	expected := make([]ycsbColumnShape, 0, fieldCount+1)
	expected = append(expected, ycsbColumnShape{
		name: "YCSB_KEY", columnType: fmt.Sprintf("varchar(%d)", ycsbKeyLength), nullable: "NO", columnKey: "PRI",
	})
	for index := 0; index < fieldCount; index++ {
		expected = append(expected, ycsbColumnShape{
			name: fmt.Sprintf("FIELD%d", index), columnType: fmt.Sprintf("varchar(%d)", fieldLength), nullable: "YES",
		})
	}
	for index := range expected {
		got := shape.columns[index]
		want := expected[index]
		if !strings.EqualFold(got.name, want.name) ||
			!strings.EqualFold(got.columnType, want.columnType) ||
			!strings.EqualFold(got.nullable, want.nullable) ||
			!strings.EqualFold(got.columnKey, want.columnKey) {
			return fmt.Errorf("YCSB column %d is %#v, want %#v", index, got, want)
		}
	}
	if len(shape.indexes) != 1 {
		return fmt.Errorf("YCSB table has %d index columns, want exactly the clustered primary key", len(shape.indexes))
	}
	index := shape.indexes[0]
	if !strings.EqualFold(index.name, "PRIMARY") || !strings.EqualFold(index.column, "YCSB_KEY") || index.sequence != 1 || index.nonUnique != 0 {
		return fmt.Errorf("YCSB primary index is %#v, want PRIMARY(YCSB_KEY)", index)
	}
	return nil
}

func readYCSBTableShape(ctx context.Context, connection *sql.Conn, database string) (ycsbTableShape, error) {
	var shape ycsbTableShape
	if err := connection.QueryRowContext(ctx, `SELECT TABLE_TYPE, COALESCE(CREATE_OPTIONS, '')
		FROM information_schema.tables WHERE table_schema = ? AND table_name = ?`, database, ycsbTableName).Scan(&shape.tableType, &shape.createOptions); err != nil {
		return shape, fmt.Errorf("read YCSB table metadata: %w", err)
	}
	var shownName string
	if err := connection.QueryRowContext(ctx, "SHOW CREATE TABLE `"+ycsbTableName+"`").Scan(&shownName, &shape.createSQL); err != nil {
		return shape, fmt.Errorf("read YCSB CREATE TABLE: %w", err)
	}
	if !strings.EqualFold(shownName, ycsbTableName) {
		return shape, fmt.Errorf("SHOW CREATE returned YCSB table %q", shownName)
	}
	var partitions int
	if err := connection.QueryRowContext(ctx, `SELECT COUNT(*) FROM information_schema.partitions
		WHERE table_schema = ? AND table_name = ? AND partition_name IS NOT NULL`, database, ycsbTableName).Scan(&partitions); err != nil {
		return shape, fmt.Errorf("read YCSB partitions: %w", err)
	}
	shape.partitioned = partitions != 0

	rows, err := connection.QueryContext(ctx, `SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_KEY
		FROM information_schema.columns WHERE table_schema = ? AND table_name = ? ORDER BY ORDINAL_POSITION`, database, ycsbTableName)
	if err != nil {
		return shape, fmt.Errorf("read YCSB columns: %w", err)
	}
	for rows.Next() {
		var column ycsbColumnShape
		if err := rows.Scan(&column.name, &column.columnType, &column.nullable, &column.columnKey); err != nil {
			rows.Close()
			return shape, fmt.Errorf("scan YCSB column: %w", err)
		}
		shape.columns = append(shape.columns, column)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return shape, fmt.Errorf("scan YCSB columns: %w", err)
	}
	if err := rows.Close(); err != nil {
		return shape, fmt.Errorf("close YCSB columns: %w", err)
	}

	rows, err = connection.QueryContext(ctx, `SELECT INDEX_NAME, COLUMN_NAME, SEQ_IN_INDEX, NON_UNIQUE
		FROM information_schema.statistics WHERE table_schema = ? AND table_name = ? ORDER BY INDEX_NAME, SEQ_IN_INDEX`, database, ycsbTableName)
	if err != nil {
		return shape, fmt.Errorf("read YCSB indexes: %w", err)
	}
	for rows.Next() {
		var index ycsbIndexShape
		if err := rows.Scan(&index.name, &index.column, &index.sequence, &index.nonUnique); err != nil {
			rows.Close()
			return shape, fmt.Errorf("scan YCSB index: %w", err)
		}
		shape.indexes = append(shape.indexes, index)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return shape, fmt.Errorf("scan YCSB indexes: %w", err)
	}
	if err := rows.Close(); err != nil {
		return shape, fmt.Errorf("close YCSB indexes: %w", err)
	}
	return shape, nil
}

type ycsbGatePlanQuery struct {
	name        string
	textSQL     string
	preparedSQL string
	arguments   []any
	mustRead    bool
}

type ycsbProbeResult struct {
	Name         string `json:"name"`
	Protocol     string `json:"protocol"`
	RowsObserved int64  `json:"rows_observed"`
}

type ycsbGateReport struct {
	FormatVersion    int               `json:"format_version"`
	Database         string            `json:"database"`
	Table            string            `json:"table"`
	FieldCount       int               `json:"field_count"`
	FieldLength      int               `json:"field_length"`
	SessionVariables map[string]string `json:"session_variables"`
	Plans            []planResult      `json:"plans"`
	Probes           []ycsbProbeResult `json:"probes"`
	Outcome          string            `json:"outcome"`
}

func sqlStringLiteral(value string) (string, error) {
	for _, character := range value {
		if !(character == '-' || character == '_' || character >= '0' && character <= '9' || character >= 'A' && character <= 'Z' || character >= 'a' && character <= 'z') {
			return "", fmt.Errorf("value %q cannot be represented by the controlled SQL literal encoder", value)
		}
	}
	return "'" + value + "'", nil
}

func ycsbInsertShape(fieldCount int, key, valuePrefix string, prepared bool) (string, []any, error) {
	columns := append([]string{"YCSB_KEY"}, ycsbColumnNames(fieldCount)...)
	if prepared {
		placeholders := make([]string, len(columns))
		arguments := make([]any, 0, len(columns))
		for index := range placeholders {
			placeholders[index] = "?"
			if index == 0 {
				arguments = append(arguments, key)
			} else {
				arguments = append(arguments, fmt.Sprintf("%s%d", valuePrefix, index-1))
			}
		}
		return "INSERT IGNORE INTO `" + ycsbTableName + "` (" + strings.Join(columns, ",") + ") VALUES (" + strings.Join(placeholders, ",") + ")", arguments, nil
	}
	values := make([]string, len(columns))
	for index := range values {
		value := key
		if index != 0 {
			value = fmt.Sprintf("%s%d", valuePrefix, index-1)
		}
		literal, err := sqlStringLiteral(value)
		if err != nil {
			return "", nil, err
		}
		values[index] = literal
	}
	return "INSERT IGNORE INTO `" + ycsbTableName + "` (" + strings.Join(columns, ",") + ") VALUES (" + strings.Join(values, ",") + ")", nil, nil
}

func ycsbGatePlanQueries(existingKey string, fieldCount int) ([]ycsbGatePlanQuery, error) {
	existingLiteral, err := sqlStringLiteral(existingKey)
	if err != nil {
		return nil, err
	}
	missingKey := "co_checker_gate_missing"
	missingLiteral, _ := sqlStringLiteral(missingKey)
	insertSQL, _, err := ycsbInsertShape(fieldCount, "co_checker_gate_plan_insert", "gate", false)
	if err != nil {
		return nil, err
	}
	preparedInsert, insertArguments, err := ycsbInsertShape(fieldCount, "co_checker_gate_plan_insert", "gate", true)
	if err != nil {
		return nil, err
	}
	fields := strings.Join(ycsbColumnNames(fieldCount), ",")
	pointPrefix := "SELECT " + fields + " FROM `" + ycsbTableName + "` FORCE INDEX(`PRIMARY`) WHERE YCSB_KEY = "
	return []ycsbGatePlanQuery{
		{name: "point_select", textSQL: pointPrefix + existingLiteral, preparedSQL: pointPrefix + "?", arguments: []any{existingKey}, mustRead: true},
		{name: "missing_select", textSQL: pointPrefix + missingLiteral, preparedSQL: pointPrefix + "?", arguments: []any{missingKey}, mustRead: true},
		{name: "point_update", textSQL: "UPDATE `" + ycsbTableName + "` SET FIELD0 = 'gate_text' WHERE YCSB_KEY = " + existingLiteral, preparedSQL: "UPDATE `" + ycsbTableName + "` SET FIELD0 = ? WHERE YCSB_KEY = ?", arguments: []any{"gate_prepared", existingKey}, mustRead: true},
		{name: "insert_ignore", textSQL: insertSQL, preparedSQL: preparedInsert, arguments: insertArguments},
		{name: "range_select", textSQL: "SELECT " + fields + " FROM `" + ycsbTableName + "` WHERE YCSB_KEY >= " + existingLiteral + " LIMIT 3", preparedSQL: "SELECT " + fields + " FROM `" + ycsbTableName + "` WHERE YCSB_KEY >= ? LIMIT ?", arguments: []any{existingKey, 3}, mustRead: true},
		{name: "point_delete", textSQL: "DELETE FROM `" + ycsbTableName + "` WHERE YCSB_KEY = " + existingLiteral, preparedSQL: "DELETE FROM `" + ycsbTableName + "` WHERE YCSB_KEY = ?", arguments: []any{existingKey}, mustRead: true},
	}, nil
}

type queryContextExecutor interface {
	QueryContext(context.Context, string, ...any) (*sql.Rows, error)
}

func consumeYCSBRows(ctx context.Context, executor queryContextExecutor, query string, fieldCount int, arguments ...any) (int64, error) {
	rows, err := executor.QueryContext(ctx, query, arguments...)
	if err != nil {
		return 0, err
	}
	return consumeYCSBResultRows(rows, fieldCount)
}

func consumePreparedYCSBRows(ctx context.Context, statement *sql.Stmt, fieldCount int, arguments ...any) (int64, error) {
	rows, err := statement.QueryContext(ctx, arguments...)
	if err != nil {
		return 0, err
	}
	return consumeYCSBResultRows(rows, fieldCount)
}

func consumeYCSBResultRows(rows *sql.Rows, fieldCount int) (int64, error) {
	defer rows.Close()
	var count int64
	for rows.Next() {
		values := make([]sql.RawBytes, fieldCount)
		destinations := make([]any, fieldCount)
		for index := range values {
			destinations[index] = &values[index]
		}
		if err := rows.Scan(destinations...); err != nil {
			return count, err
		}
		count++
	}
	if err := rows.Err(); err != nil {
		return count, err
	}
	return count, rows.Close()
}

func requireAffected(result sql.Result, operation string) (int64, error) {
	count, err := result.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("%s rows affected: %w", operation, err)
	}
	if count != 1 {
		return count, fmt.Errorf("%s affected %d rows, want 1", operation, count)
	}
	return count, nil
}

func ycsbGate(ctx context.Context, connection *sql.Conn, database string, fieldCount, fieldLength int) error {
	if !identifierPattern.MatchString(database) {
		return fmt.Errorf("invalid database identifier %q", database)
	}
	shape, err := readYCSBTableShape(ctx, connection, database)
	if err != nil {
		return err
	}
	if err := validateYCSBTableShape(shape, fieldCount, fieldLength); err != nil {
		return fmt.Errorf("YCSB capability gate: %w", err)
	}
	variables, err := readCheckedSessionVariables(ctx, connection)
	if err != nil {
		return err
	}
	if err := validateYCSBSessionVariables("YCSB capability gate", variables); err != nil {
		return err
	}

	var existingKey string
	if err := connection.QueryRowContext(ctx, "SELECT YCSB_KEY FROM `"+ycsbTableName+"` ORDER BY YCSB_KEY LIMIT 1").Scan(&existingKey); err != nil {
		return fmt.Errorf("YCSB capability gate requires a loaded row: %w", err)
	}
	planQueries, err := ycsbGatePlanQueries(existingKey, fieldCount)
	if err != nil {
		return fmt.Errorf("YCSB capability gate plan shapes: %w", err)
	}
	report := ycsbGateReport{
		FormatVersion:    1,
		Database:         database,
		Table:            ycsbTableName,
		FieldCount:       fieldCount,
		FieldLength:      fieldLength,
		SessionVariables: variables,
		Plans:            make([]planResult, 0, 2*len(planQueries)),
	}
	for _, query := range planQueries {
		operators, rows, err := explainPlan(ctx, connection, query.textSQL)
		if err != nil {
			return fmt.Errorf("YCSB capability gate: explain %s text: %w", query.name, err)
		}
		if err := validateTPCPlan(query.name+"_text", operators, rows, query.mustRead); err != nil {
			return err
		}
		report.Plans = append(report.Plans, planResult{Name: query.name + "_text", Operators: operators, Rows: rows})
		operators, rows, err = explainPlan(ctx, connection, query.preparedSQL, query.arguments...)
		if err != nil {
			return fmt.Errorf("YCSB capability gate: explain %s prepared: %w", query.name, err)
		}
		if err := validateTPCPlan(query.name+"_prepared", operators, rows, query.mustRead); err != nil {
			return err
		}
		report.Plans = append(report.Plans, planResult{Name: query.name + "_prepared", Operators: operators, Rows: rows})
	}

	fields := strings.Join(ycsbColumnNames(fieldCount), ",")
	existingLiteral, _ := sqlStringLiteral(existingKey)
	missingKey := "co_checker_gate_missing"
	missingLiteral, _ := sqlStringLiteral(missingKey)
	pointPrefix := "SELECT " + fields + " FROM `" + ycsbTableName + "` FORCE INDEX(`PRIMARY`) WHERE YCSB_KEY = "
	textCount, err := consumeYCSBRows(ctx, connection, pointPrefix+existingLiteral, fieldCount)
	if err != nil || textCount != 1 {
		return fmt.Errorf("YCSB text point probe observed %d rows: %w", textCount, err)
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "point_select", Protocol: "text", RowsObserved: textCount})
	preparedPoint, err := connection.PrepareContext(ctx, pointPrefix+"?")
	if err != nil {
		return fmt.Errorf("prepare YCSB point probe: %w", err)
	}
	preparedCount, queryErr := consumePreparedYCSBRows(ctx, preparedPoint, fieldCount, existingKey)
	closeErr := preparedPoint.Close()
	if queryErr != nil || closeErr != nil || preparedCount != 1 {
		return fmt.Errorf("YCSB prepared point probe observed %d rows: query=%v close=%v", preparedCount, queryErr, closeErr)
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "point_select", Protocol: "prepared", RowsObserved: preparedCount})

	textCount, err = consumeYCSBRows(ctx, connection, pointPrefix+missingLiteral, fieldCount)
	if err != nil || textCount != 0 {
		return fmt.Errorf("YCSB text missing probe observed %d rows: %w", textCount, err)
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "missing_select", Protocol: "text", RowsObserved: textCount})
	preparedMissing, err := connection.PrepareContext(ctx, pointPrefix+"?")
	if err != nil {
		return fmt.Errorf("prepare YCSB missing probe: %w", err)
	}
	preparedCount, queryErr = consumePreparedYCSBRows(ctx, preparedMissing, fieldCount, missingKey)
	closeErr = preparedMissing.Close()
	if queryErr != nil || closeErr != nil || preparedCount != 0 {
		return fmt.Errorf("YCSB prepared missing probe observed %d rows: query=%v close=%v", preparedCount, queryErr, closeErr)
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "missing_select", Protocol: "prepared", RowsObserved: preparedCount})

	rangeText := "SELECT " + fields + " FROM `" + ycsbTableName + "` WHERE YCSB_KEY >= " + existingLiteral + " LIMIT 3"
	textCount, err = consumeYCSBRows(ctx, connection, rangeText, fieldCount)
	if err != nil || textCount < 1 || textCount > 3 {
		return fmt.Errorf("YCSB text range probe observed %d rows: %w", textCount, err)
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "range_select", Protocol: "text", RowsObserved: textCount})
	preparedRange, err := connection.PrepareContext(ctx, "SELECT "+fields+" FROM `"+ycsbTableName+"` WHERE YCSB_KEY >= ? LIMIT ?")
	if err != nil {
		return fmt.Errorf("prepare YCSB range probe: %w", err)
	}
	preparedCount, queryErr = consumePreparedYCSBRows(ctx, preparedRange, fieldCount, existingKey, 3)
	closeErr = preparedRange.Close()
	if queryErr != nil || closeErr != nil || preparedCount < 1 || preparedCount > 3 {
		return fmt.Errorf("YCSB prepared range probe observed %d rows: query=%v close=%v", preparedCount, queryErr, closeErr)
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "range_select", Protocol: "prepared", RowsObserved: preparedCount})

	if err := runYCSBMutatingProbes(ctx, connection, existingKey, fieldCount, &report); err != nil {
		return err
	}
	report.Outcome = "pass"
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(report)
}

func runYCSBMutatingProbes(ctx context.Context, connection *sql.Conn, existingKey string, fieldCount int, report *ycsbGateReport) error {
	tx, err := connection.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin YCSB mutating probes: %w", err)
	}
	defer tx.Rollback()
	existingLiteral, _ := sqlStringLiteral(existingKey)
	result, err := tx.ExecContext(ctx, "UPDATE `"+ycsbTableName+"` SET FIELD0 = 'gate_text' WHERE YCSB_KEY = "+existingLiteral)
	if err != nil {
		return fmt.Errorf("YCSB text update probe: %w", err)
	}
	count, err := requireAffected(result, "YCSB text update probe")
	if err != nil {
		return err
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "point_update", Protocol: "text", RowsObserved: count})
	prepared, err := tx.PrepareContext(ctx, "UPDATE `"+ycsbTableName+"` SET FIELD0 = ? WHERE YCSB_KEY = ?")
	if err != nil {
		return fmt.Errorf("prepare YCSB update probe: %w", err)
	}
	result, execErr := prepared.ExecContext(ctx, "gate_prepared", existingKey)
	closeErr := prepared.Close()
	if execErr != nil || closeErr != nil {
		return fmt.Errorf("YCSB prepared update probe: execute=%v close=%v", execErr, closeErr)
	}
	count, err = requireAffected(result, "YCSB prepared update probe")
	if err != nil {
		return err
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "point_update", Protocol: "prepared", RowsObserved: count})

	textInsertKey := "co_checker_gate_text_insert"
	preparedInsertKey := "co_checker_gate_prepared_insert"
	textInsert, _, err := ycsbInsertShape(fieldCount, textInsertKey, "text", false)
	if err != nil {
		return err
	}
	result, err = tx.ExecContext(ctx, textInsert)
	if err != nil {
		return fmt.Errorf("YCSB text insert probe: %w", err)
	}
	count, err = requireAffected(result, "YCSB text insert probe")
	if err != nil {
		return err
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "insert_ignore", Protocol: "text", RowsObserved: count})
	preparedInsert, insertArguments, err := ycsbInsertShape(fieldCount, preparedInsertKey, "prepared", true)
	if err != nil {
		return err
	}
	prepared, err = tx.PrepareContext(ctx, preparedInsert)
	if err != nil {
		return fmt.Errorf("prepare YCSB insert probe: %w", err)
	}
	result, execErr = prepared.ExecContext(ctx, insertArguments...)
	closeErr = prepared.Close()
	if execErr != nil || closeErr != nil {
		return fmt.Errorf("YCSB prepared insert probe: execute=%v close=%v", execErr, closeErr)
	}
	count, err = requireAffected(result, "YCSB prepared insert probe")
	if err != nil {
		return err
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "insert_ignore", Protocol: "prepared", RowsObserved: count})

	textInsertLiteral, _ := sqlStringLiteral(textInsertKey)
	result, err = tx.ExecContext(ctx, "DELETE FROM `"+ycsbTableName+"` WHERE YCSB_KEY = "+textInsertLiteral)
	if err != nil {
		return fmt.Errorf("YCSB text delete probe: %w", err)
	}
	count, err = requireAffected(result, "YCSB text delete probe")
	if err != nil {
		return err
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "point_delete", Protocol: "text", RowsObserved: count})
	prepared, err = tx.PrepareContext(ctx, "DELETE FROM `"+ycsbTableName+"` WHERE YCSB_KEY = ?")
	if err != nil {
		return fmt.Errorf("prepare YCSB delete probe: %w", err)
	}
	result, execErr = prepared.ExecContext(ctx, preparedInsertKey)
	closeErr = prepared.Close()
	if execErr != nil || closeErr != nil {
		return fmt.Errorf("YCSB prepared delete probe: execute=%v close=%v", execErr, closeErr)
	}
	count, err = requireAffected(result, "YCSB prepared delete probe")
	if err != nil {
		return err
	}
	report.Probes = append(report.Probes, ycsbProbeResult{Name: "point_delete", Protocol: "prepared", RowsObserved: count})
	if err := tx.Rollback(); err != nil {
		return fmt.Errorf("rollback YCSB mutating probes: %w", err)
	}
	return nil
}

func tpccGate(ctx context.Context, connection *sql.Conn) error {
	variables, err := readCheckedSessionVariables(ctx, connection)
	if err != nil {
		return err
	}
	if err := validateCheckedSessionVariables("capability gate", variables); err != nil {
		return err
	}

	queries := tpccPlanQueries()
	report := gateReport{SessionVariables: variables, Plans: make([]planResult, 0, 2*len(queries))}
	for _, query := range queries {
		operators, rows, err := explainPlan(ctx, connection, query.sql)
		if err != nil {
			return fmt.Errorf("capability gate: explain %s: %w", query.name, err)
		}
		if err := validateTPCPlan(query.name, operators, rows, query.mustRead); err != nil {
			return err
		}
		report.Plans = append(report.Plans, planResult{Name: query.name, Operators: operators, Rows: rows})

		preparedSQL, arguments, err := parameterizeFirstIntegerLiteral(query.sql)
		if err != nil {
			return fmt.Errorf("capability gate: prepare %s: %w", query.name, err)
		}
		preparedName := query.name + "_prepared"
		operators, rows, err = explainPlan(ctx, connection, preparedSQL, arguments...)
		if err != nil {
			return fmt.Errorf("capability gate: explain %s: %w", preparedName, err)
		}
		if err := validateTPCPlan(preparedName, operators, rows, query.mustRead); err != nil {
			return err
		}
		report.Plans = append(report.Plans, planResult{Name: preparedName, Operators: operators, Rows: rows})
	}
	report.Outcome = "pass"
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(report)
}

func readCheckedSessionVariables(ctx context.Context, connection *sql.Conn) (map[string]string, error) {
	const query = `SELECT @@transaction_isolation, @@tidb_txn_mode,
		@@tidb_enable_async_commit, @@tidb_enable_1pc,
		@@tidb_low_resolution_tso, @@tidb_read_staleness, @@tidb_snapshot,
		@@tidb_store_batch_size, @@tidb_allow_batch_cop,
		@@tidb_index_lookup_pushdown_policy, @@tidb_retry_limit`
	values := make([]sql.NullString, 11)
	arguments := make([]any, len(values))
	for i := range values {
		arguments[i] = &values[i]
	}
	if err := connection.QueryRowContext(ctx, query).Scan(arguments...); err != nil {
		return nil, fmt.Errorf("read checked session variables: %w", err)
	}
	names := []string{
		"transaction_isolation", "tidb_txn_mode", "tidb_enable_async_commit",
		"tidb_enable_1pc", "tidb_low_resolution_tso", "tidb_read_staleness", "tidb_snapshot",
		"tidb_store_batch_size", "tidb_allow_batch_cop",
		"tidb_index_lookup_pushdown_policy", "tidb_retry_limit",
	}
	result := make(map[string]string, len(names))
	for i, name := range names {
		if values[i].Valid {
			result[name] = values[i].String
		}
	}
	return result, nil
}

func validateCheckedSessionVariables(gate string, variables map[string]string) error {
	for name, want := range expectedCheckedSessionVariables {
		got, exists := variables[name]
		if !exists {
			return fmt.Errorf("%s: session variable %s is missing", gate, name)
		}
		if strings.ToUpper(got) != want {
			return fmt.Errorf("%s: %s=%q, want %q", gate, name, got, want)
		}
	}
	return nil
}

func validateYCSBSessionVariables(gate string, variables map[string]string) error {
	if err := validateCheckedSessionVariables(gate, variables); err != nil {
		return err
	}
	got, exists := variables["tidb_retry_limit"]
	if !exists {
		return fmt.Errorf("%s: session variable tidb_retry_limit is missing", gate)
	}
	if got != ycsbRetryLimit {
		return fmt.Errorf("%s: tidb_retry_limit=%q, want %q", gate, got, ycsbRetryLimit)
	}
	return nil
}

type namedPlanQuery struct {
	name     string
	sql      string
	mustRead bool
}

func tpccPlanQueries() []namedPlanQuery {
	queries := []namedPlanQuery{
		{name: "new_order_select_customer_and_warehouse", sql: `SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse WHERE w_id = 1 AND c_w_id = w_id AND c_d_id = 1 AND c_id = 1`, mustRead: true},
		{name: "new_order_select_district_for_update", sql: `SELECT d_next_o_id, d_tax FROM district WHERE d_id = 1 AND d_w_id = 1 FOR UPDATE`, mustRead: true},
		{name: "new_order_update_district", sql: `UPDATE district SET d_next_o_id = 3002 WHERE d_id = 1 AND d_w_id = 1`, mustRead: true},
		{name: "new_order_insert_order", sql: `INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) VALUES (3001, 1, 1, 1, '2024-01-01 00:00:00', 5, 1)`},
		{name: "new_order_insert_new_order", sql: `INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES (3001, 1, 1)`},
		{name: "new_order_update_stock", sql: `UPDATE stock SET s_quantity = 50, s_ytd = s_ytd + 5, s_order_cnt = s_order_cnt + 1, s_remote_cnt = s_remote_cnt + 0 WHERE s_i_id = 1 AND s_w_id = 1`, mustRead: true},

		{name: "payment_update_district", sql: `UPDATE district SET d_ytd = d_ytd + 1.00 WHERE d_w_id = 1 AND d_id = 1`, mustRead: true},
		{name: "payment_select_district", sql: `SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name FROM district WHERE d_w_id = 1 AND d_id = 1`, mustRead: true},
		{name: "payment_update_warehouse", sql: `UPDATE warehouse SET w_ytd = w_ytd + 1.00 WHERE w_id = 1`, mustRead: true},
		{name: "payment_select_warehouse", sql: `SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name FROM warehouse WHERE w_id = 1`, mustRead: true},
		{name: "payment_customer_by_last_name", sql: `SELECT c_id FROM customer WHERE c_w_id = 1 AND c_d_id = 1 AND c_last = 'BARBARBAR' ORDER BY c_first`, mustRead: true},
		{name: "payment_customer_by_id_for_update", sql: `SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = 1 AND c_d_id = 1 AND c_id = 1 FOR UPDATE`, mustRead: true},
		{name: "payment_select_customer_data", sql: `SELECT c_data FROM customer WHERE c_w_id = 1 AND c_d_id = 1 AND c_id = 1`, mustRead: true},
		{name: "payment_update_customer", sql: `UPDATE customer SET c_balance = c_balance - 1.00, c_ytd_payment = c_ytd_payment + 1.00, c_payment_cnt = c_payment_cnt + 1 WHERE c_w_id = 1 AND c_d_id = 1 AND c_id = 1`, mustRead: true},
		{name: "payment_update_customer_with_data", sql: `UPDATE customer SET c_balance = c_balance - 1.00, c_ytd_payment = c_ytd_payment + 1.00, c_payment_cnt = c_payment_cnt + 1, c_data = 'gate' WHERE c_w_id = 1 AND c_d_id = 1 AND c_id = 1`, mustRead: true},
		{name: "payment_insert_history", sql: `INSERT INTO history (h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data) VALUES (1, 1, 1, 1, 1, '2024-01-01 00:00:00', 1.00, 'gate')`},

		{name: "order_status_customer_count_by_last_name", sql: `SELECT count(c_id) namecnt FROM customer WHERE c_w_id = 1 AND c_d_id = 1 AND c_last = 'BARBARBAR'`, mustRead: true},
		{name: "order_status_customer_by_last_name", sql: `SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE c_w_id = 1 AND c_d_id = 1 AND c_last = 'BARBARBAR' ORDER BY c_first`, mustRead: true},
		{name: "order_status_customer_by_id", sql: `SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_w_id = 1 AND c_d_id = 1 AND c_id = 1`, mustRead: true},
		{name: "order_status_latest_order", sql: `SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_w_id = 1 AND o_d_id = 1 AND o_c_id = 1 ORDER BY o_id DESC LIMIT 1`, mustRead: true},
		{name: "order_status_order_line", sql: `SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_w_id = 1 AND ol_d_id = 1 AND ol_o_id = 1`, mustRead: true},

		{name: "delivery_oldest_new_order", sql: `SELECT no_o_id FROM new_order WHERE no_w_id = 1 AND no_d_id = 1 ORDER BY no_o_id ASC LIMIT 1 FOR UPDATE`, mustRead: true},

		{name: "stock_level_select_district", sql: `SELECT d_next_o_id FROM district WHERE d_w_id = 1 AND d_id = 1`, mustRead: true},
		{name: "stock_level_count", sql: `SELECT /*+ TIDB_INLJ(order_line,stock) */ COUNT(DISTINCT (s_i_id)) stock_count FROM order_line, stock WHERE ol_w_id = 1 AND ol_d_id = 1 AND ol_o_id < 3001 AND ol_o_id >= 3001 - 20 AND s_w_id = 1 AND s_i_id = ol_i_id AND s_quantity < 15`, mustRead: true},
	}

	deliveryTupleList := make([]string, 10)
	for districtID := 1; districtID <= 10; districtID++ {
		deliveryTupleList[districtID-1] = fmt.Sprintf("(1,%d,2101)", districtID)
	}
	deliveryTuples := strings.Join(deliveryTupleList, ",")
	queries = append(queries,
		namedPlanQuery{name: "delivery_delete_new_orders", sql: `DELETE FROM new_order WHERE (no_w_id, no_d_id, no_o_id) IN (` + deliveryTuples + `)`, mustRead: true},
		namedPlanQuery{name: "delivery_update_orders", sql: `UPDATE orders SET o_carrier_id = 1 WHERE (o_w_id, o_d_id, o_id) IN (` + deliveryTuples + `)`, mustRead: true},
		namedPlanQuery{name: "delivery_select_orders", sql: `SELECT o_d_id, o_c_id FROM orders WHERE (o_w_id, o_d_id, o_id) IN (` + deliveryTuples + `)`, mustRead: true},
		namedPlanQuery{name: "delivery_update_order_line", sql: `UPDATE order_line SET ol_delivery_d = '2024-01-01 00:00:00' WHERE (ol_w_id, ol_d_id, ol_o_id) IN (` + deliveryTuples + `)`, mustRead: true},
		namedPlanQuery{name: "delivery_sum_order_line", sql: `SELECT ol_d_id, SUM(ol_amount) FROM order_line WHERE (ol_w_id, ol_d_id, ol_o_id) IN (` + deliveryTuples + `) GROUP BY ol_d_id`, mustRead: true},
		namedPlanQuery{name: "delivery_update_customer", sql: `UPDATE customer SET c_balance = c_balance + 1.00, c_delivery_cnt = c_delivery_cnt + 1 WHERE c_w_id = 1 AND c_d_id = 1 AND c_id = 1`, mustRead: true},
	)

	for count := 5; count <= 15; count++ {
		itemIDs := make([]string, count)
		stockIDs := make([]string, count)
		orderLineValues := make([]string, count)
		for i := 0; i < count; i++ {
			id := strconv.Itoa(i + 1)
			itemIDs[i] = id
			stockIDs[i] = "(1," + id + ")"
			orderLineValues[i] = fmt.Sprintf("(1,1,1,%d,%s,1,5,1.00,'gate')", i+1, id)
		}
		queries = append(queries,
			namedPlanQuery{name: fmt.Sprintf("new_order_select_items_%d", count), sql: `SELECT i_price, i_name, i_data, i_id FROM item WHERE i_id IN (` + strings.Join(itemIDs, ",") + `)`, mustRead: true},
			namedPlanQuery{name: fmt.Sprintf("new_order_select_stock_%d_for_update", count), sql: `SELECT s_i_id, s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 FROM stock WHERE (s_w_id, s_i_id) IN (` + strings.Join(stockIDs, ",") + `) FOR UPDATE`, mustRead: true},
			namedPlanQuery{name: fmt.Sprintf("new_order_insert_order_line_%d", count), sql: `INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES ` + strings.Join(orderLineValues, ",")},
		)
	}
	return queries
}

func isSQLIdentifierByte(value byte) bool {
	return value == '_' || value == '$' ||
		value >= '0' && value <= '9' ||
		value >= 'a' && value <= 'z' ||
		value >= 'A' && value <= 'Z'
}

// parameterizeFirstIntegerLiteral produces a representative prepared-protocol
// variant without changing the plan shape. Quoted strings and decimal
// components are deliberately skipped.
func parameterizeFirstIntegerLiteral(query string) (string, []any, error) {
	inString := false
	for index := 0; index < len(query); index++ {
		if query[index] == '\'' {
			if inString && index+1 < len(query) && query[index+1] == '\'' {
				index++
				continue
			}
			inString = !inString
			continue
		}
		if inString || query[index] < '0' || query[index] > '9' {
			continue
		}
		if index > 0 && (isSQLIdentifierByte(query[index-1]) || query[index-1] == '.') {
			continue
		}
		end := index + 1
		for end < len(query) && query[end] >= '0' && query[end] <= '9' {
			end++
		}
		if end < len(query) && (isSQLIdentifierByte(query[end]) || query[end] == '.') {
			index = end - 1
			continue
		}
		value, err := strconv.ParseInt(query[index:end], 10, 64)
		if err != nil {
			return "", nil, err
		}
		return query[:index] + "?" + query[end:], []any{value}, nil
	}
	return "", nil, errors.New("SQL contains no parameterizable integer literal")
}

func explainPlan(ctx context.Context, connection *sql.Conn, query string, arguments ...any) ([]string, [][]string, error) {
	rows, err := connection.QueryContext(ctx, "EXPLAIN FORMAT='brief' "+query, arguments...)
	if err != nil {
		return nil, nil, err
	}
	defer rows.Close()
	columns, err := rows.Columns()
	if err != nil {
		return nil, nil, err
	}
	if len(columns) == 0 {
		return nil, nil, errors.New("EXPLAIN returned no columns")
	}
	operators := make([]string, 0, 8)
	explainRows := make([][]string, 0, 8)
	for rows.Next() {
		values := make([]sql.NullString, len(columns))
		destinations := make([]any, len(values))
		for i := range values {
			destinations[i] = &values[i]
		}
		if err := rows.Scan(destinations...); err != nil {
			return nil, nil, err
		}
		explainRow := make([]string, len(values))
		for index := range values {
			if values[index].Valid {
				explainRow[index] = values[index].String
			}
		}
		explainRows = append(explainRows, explainRow)
		if values[0].Valid {
			operators = append(operators, values[0].String)
		}
	}
	if err := rows.Err(); err != nil {
		return nil, nil, err
	}
	if len(operators) == 0 {
		return nil, nil, errors.New("EXPLAIN returned no plan rows")
	}
	return operators, explainRows, nil
}

func validateTPCPlan(name string, operators []string, rows [][]string, mustRead bool) error {
	readEvidencePlan := false
	indexLookupCount := 0
	for _, operator := range operators {
		normalized := strings.ToLower(operator)
		if strings.Contains(normalized, "indexlookup") {
			indexLookupCount++
		}
		for _, supported := range []string{
			"point_get", "batch_point_get", "tablereader", "indexreader",
			"indexlookup", "tablescan", "indexscan",
		} {
			if strings.Contains(normalized, supported) {
				readEvidencePlan = true
			}
		}
		for _, unsupported := range []string{
			"indexmerge", "tiflash", "exchange", "mpp", "batchcop", "streaming",
		} {
			if strings.Contains(normalized, unsupported) {
				return fmt.Errorf("capability gate: %s uses unsupported operator %q", name, operator)
			}
		}
	}
	for _, row := range rows {
		description := strings.ToLower(strings.Join(row, " "))
		for _, unsupported := range []string{
			"indexmerge", "tiflash", "exchange", "mpp", "batchcop", "streaming",
		} {
			if strings.Contains(description, unsupported) {
				return fmt.Errorf("capability gate: %s uses unsupported plan row %q", name, row)
			}
		}
	}
	if mustRead && !readEvidencePlan {
		return fmt.Errorf("capability gate: %s has no supported point/reader evidence path: %v", name, operators)
	}
	if indexLookupCount > 0 {
		zeroTableSideScans := 0
		for _, row := range rows {
			if len(row) < 3 || !strings.Contains(strings.ToLower(row[2]), "cop") {
				continue
			}
			operator := strings.ToLower(row[0])
			tableSide := strings.Contains(operator, "tablerowidscan") ||
				strings.Contains(operator, "tablerangescan") ||
				strings.Contains(operator, "tablefullscan")
			if !tableSide {
				continue
			}
			estimatedRows, err := strconv.ParseFloat(row[1], 64)
			if err == nil && estimatedRows == 0 {
				zeroTableSideScans++
			}
		}
		if zeroTableSideScans < indexLookupCount {
			return fmt.Errorf("capability gate: %s has %d IndexLookUp reader(s), but only %d pushed table-side scans: %v", name, indexLookupCount, zeroTableSideScans, rows)
		}
	}
	return nil
}
