// tidb_sql is a deliberately small SQL control utility for the online TPC-C
// workflow. It is run from the TiDB module so it reuses TiDB's pinned MySQL
// driver instead of adding another dependency to co-checker.
package main

import (
	"context"
	"database/sql"
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
	Name      string   `json:"name"`
	Operators []string `json:"operators"`
}

type gateReport struct {
	SessionVariables map[string]string `json:"session_variables"`
	Plans            []planResult      `json:"plans"`
	Outcome          string            `json:"outcome"`
}

func main() {
	var cfg options
	flag.StringVar(&cfg.action, "action", "ping", "ping, exec, assert-empty, or new-order-gate")
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
	case "new-order-gate":
		return newOrderGate(ctx, connection)
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

func newOrderGate(ctx context.Context, connection *sql.Conn) error {
	variables, err := readCheckedSessionVariables(ctx, connection)
	if err != nil {
		return err
	}
	expected := map[string]string{
		"transaction_isolation":    "REPEATABLE-READ",
		"tidb_txn_mode":            "OPTIMISTIC",
		"tidb_enable_async_commit": "0",
		"tidb_enable_1pc":          "0",
		"tidb_low_resolution_tso":  "0",
		"tidb_read_staleness":      "0",
		"tidb_snapshot":            "",
	}
	for name, want := range expected {
		if got := strings.ToUpper(variables[name]); got != want {
			return fmt.Errorf("capability gate: %s=%q, want %q", name, variables[name], want)
		}
	}

	queries := newOrderPlanQueries()
	report := gateReport{SessionVariables: variables, Plans: make([]planResult, 0, len(queries))}
	for _, query := range queries {
		operators, err := explainOperators(ctx, connection, query.sql)
		if err != nil {
			return fmt.Errorf("capability gate: explain %s: %w", query.name, err)
		}
		if err := validateNewOrderPlan(query.name, operators, query.mustRead); err != nil {
			return err
		}
		report.Plans = append(report.Plans, planResult{Name: query.name, Operators: operators})
	}
	report.Outcome = "pass"
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(report)
}

func readCheckedSessionVariables(ctx context.Context, connection *sql.Conn) (map[string]string, error) {
	const query = `SELECT @@transaction_isolation, @@tidb_txn_mode,
		@@tidb_enable_async_commit, @@tidb_enable_1pc,
		@@tidb_low_resolution_tso, @@tidb_read_staleness, @@tidb_snapshot`
	values := make([]sql.NullString, 7)
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

func newOrderPlanQueries() []namedPlanQuery {
	queries := []namedPlanQuery{
		{
			name: "select_customer_and_warehouse",
			sql: `SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse
				WHERE w_id = 1 AND c_w_id = w_id AND c_d_id = 1 AND c_id = 1`,
			mustRead: true,
		},
		{
			name:     "select_district_for_update",
			sql:      `SELECT d_next_o_id, d_tax FROM district WHERE d_id = 1 AND d_w_id = 1 FOR UPDATE`,
			mustRead: true,
		},
		{
			name:     "update_district",
			sql:      `UPDATE district SET d_next_o_id = 3002 WHERE d_id = 1 AND d_w_id = 1`,
			mustRead: true,
		},
		{
			name:     "update_stock",
			sql:      `UPDATE stock SET s_quantity = 50, s_ytd = s_ytd + 5, s_order_cnt = s_order_cnt + 1, s_remote_cnt = s_remote_cnt + 0 WHERE s_i_id = 1 AND s_w_id = 1`,
			mustRead: true,
		},
	}
	for count := 5; count <= 15; count++ {
		itemIDs := make([]string, count)
		stockIDs := make([]string, count)
		for i := 0; i < count; i++ {
			id := strconv.Itoa(i + 1)
			itemIDs[i] = id
			stockIDs[i] = "(1," + id + ")"
		}
		queries = append(queries,
			namedPlanQuery{
				name:     fmt.Sprintf("select_items_%d", count),
				sql:      `SELECT i_price, i_name, i_data, i_id FROM item WHERE i_id IN (` + strings.Join(itemIDs, ",") + `)`,
				mustRead: true,
			},
			namedPlanQuery{
				name: fmt.Sprintf("select_stock_%d_for_update", count),
				sql: `SELECT s_i_id, s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03,
					s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10
					FROM stock WHERE (s_w_id, s_i_id) IN (` + strings.Join(stockIDs, ",") + `) FOR UPDATE`,
				mustRead: true,
			},
		)
	}
	return queries
}

func explainOperators(ctx context.Context, connection *sql.Conn, query string) ([]string, error) {
	rows, err := connection.QueryContext(ctx, "EXPLAIN FORMAT='brief' "+query)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	columns, err := rows.Columns()
	if err != nil {
		return nil, err
	}
	if len(columns) == 0 {
		return nil, errors.New("EXPLAIN returned no columns")
	}
	operators := make([]string, 0, 8)
	for rows.Next() {
		values := make([]sql.NullString, len(columns))
		destinations := make([]any, len(values))
		for i := range values {
			destinations[i] = &values[i]
		}
		if err := rows.Scan(destinations...); err != nil {
			return nil, err
		}
		if values[0].Valid {
			operators = append(operators, values[0].String)
		}
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(operators) == 0 {
		return nil, errors.New("EXPLAIN returned no plan rows")
	}
	return operators, nil
}

func validateNewOrderPlan(name string, operators []string, mustRead bool) error {
	readEvidencePlan := false
	for _, operator := range operators {
		normalized := strings.ToLower(operator)
		if strings.Contains(normalized, "point_get") || strings.Contains(normalized, "batch_point_get") {
			readEvidencePlan = true
		}
		for _, unsupported := range []string{
			"tablereader", "indexreader", "indexlookup", "indexmerge",
			"tablescan", "indexscan",
		} {
			if strings.Contains(normalized, unsupported) {
				return fmt.Errorf("capability gate: %s uses unsupported coprocessor operator %q", name, operator)
			}
		}
	}
	if mustRead && !readEvidencePlan {
		return fmt.Errorf("capability gate: %s has no Point_Get or Batch_Point_Get evidence path: %v", name, operators)
	}
	return nil
}
