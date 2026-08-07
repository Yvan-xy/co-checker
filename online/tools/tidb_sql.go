// tidb_sql is a deliberately small SQL control utility for the online TPC-C
// workflow. It is run from the TiDB module so it reuses TiDB's pinned MySQL
// driver instead of adding another dependency to co-checker.
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

var identifierPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

type options struct {
	action    string
	address   string
	user      string
	password  string
	database  string
	params    string
	tables    string
	statement string
	timeout   time.Duration
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
	flag.StringVar(&cfg.action, "action", "ping", "ping, exec, assert-empty, tpcc-digest, or tpcc-gate")
	flag.StringVar(&cfg.address, "address", "127.0.0.1:4000", "TiDB SQL address")
	flag.StringVar(&cfg.user, "user", "root", "SQL user")
	flag.StringVar(&cfg.password, "password", "", "SQL password")
	flag.StringVar(&cfg.database, "database", "tpcc", "database name")
	flag.StringVar(&cfg.params, "params", "", "additional MySQL DSN parameters")
	flag.StringVar(&cfg.tables, "tables", tpccTables, "comma-separated table names")
	flag.StringVar(&cfg.statement, "statement", "", "SQL statement for the exec action")
	flag.DurationVar(&cfg.timeout, "timeout", 30*time.Second, "operation timeout")
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

	dsn := fmt.Sprintf("%s:%s@tcp(%s)/%s?parseTime=true&multiStatements=true&tls=false",
		cfg.user, cfg.password, cfg.address, cfg.database)
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

func tpccGate(ctx context.Context, connection *sql.Conn) error {
	variables, err := readCheckedSessionVariables(ctx, connection)
	if err != nil {
		return err
	}
	expected := map[string]string{
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
	for name, want := range expected {
		if got := strings.ToUpper(variables[name]); got != want {
			return fmt.Errorf("capability gate: %s=%q, want %q", name, variables[name], want)
		}
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
		@@tidb_index_lookup_pushdown_policy`
	values := make([]sql.NullString, 10)
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
		"tidb_index_lookup_pushdown_policy",
	}
	result := make(map[string]string, len(names))
	for i, name := range names {
		if values[i].Valid {
			result[name] = values[i].String
		}
	}
	return result, nil
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
