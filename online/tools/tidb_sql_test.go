package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"testing"
)

func TestYCSBCreateTableSQL(t *testing.T) {
	query, err := ycsbCreateTableSQL("ycsb", 3, 17)
	if err != nil {
		t.Fatal(err)
	}
	want := "CREATE TABLE IF NOT EXISTS `ycsb`.`usertable` (YCSB_KEY VARCHAR(64) PRIMARY KEY /*T![clustered_index] CLUSTERED */, FIELD0 VARCHAR(17), FIELD1 VARCHAR(17), FIELD2 VARCHAR(17));"
	if query != want {
		t.Fatalf("create SQL = %q, want %q", query, want)
	}
	for _, test := range []struct {
		database    string
		fieldCount  int
		fieldLength int
	}{
		{database: "bad-name", fieldCount: 3, fieldLength: 17},
		{database: "ycsb", fieldCount: 0, fieldLength: 17},
		{database: "ycsb", fieldCount: 3, fieldLength: 0},
	} {
		if _, err := ycsbCreateTableSQL(test.database, test.fieldCount, test.fieldLength); err == nil {
			t.Fatalf("ycsbCreateTableSQL(%q,%d,%d) unexpectedly succeeded", test.database, test.fieldCount, test.fieldLength)
		}
	}
}

func TestYCSBDigestInvariantAcrossPageSizes(t *testing.T) {
	rows := []ycsbDigestRow{
		{key: []byte("user1"), fields: []ycsbDigestCell{{present: true, value: []byte("a")}, {present: false}}},
		{key: []byte("user2"), fields: []ycsbDigestCell{{present: true, value: []byte{}}, {present: true, value: []byte("b")}}},
		{key: []byte("user3"), fields: []ycsbDigestCell{{present: true, value: []byte("c")}, {present: true, value: []byte("d")}}},
	}
	var baseline ycsbDigestReport
	for _, pageSize := range []int{1, 2, 4} {
		var observedLimits []int
		fetch := func(afterKey string, limit int) ([]ycsbDigestRow, error) {
			observedLimits = append(observedLimits, limit)
			start := sort.Search(len(rows), func(index int) bool {
				return string(rows[index].key) > afterKey
			})
			end := start + limit
			if end > len(rows) {
				end = len(rows)
			}
			return rows[start:end], nil
		}
		report, err := buildYCSBDigest("CREATE TABLE `usertable` (...) CLUSTERED", 2, pageSize, fetch)
		if err != nil {
			t.Fatalf("page size %d: %v", pageSize, err)
		}
		if report.TotalRows != uint64(len(rows)) {
			t.Fatalf("page size %d row count = %d, want %d", pageSize, report.TotalRows, len(rows))
		}
		if report.MaximumObservedPage > pageSize {
			t.Fatalf("page size %d observed page %d", pageSize, report.MaximumObservedPage)
		}
		for _, limit := range observedLimits {
			if limit != pageSize {
				t.Fatalf("page size %d fetch limit = %d", pageSize, limit)
			}
		}
		if baseline.Digest == "" {
			baseline = report
			continue
		}
		if report.SchemaDigest != baseline.SchemaDigest || report.DataDigest != baseline.DataDigest || report.Digest != baseline.Digest {
			t.Fatalf("page size %d changed digest: %#v vs %#v", pageSize, report, baseline)
		}
	}
}

func validYCSBTableShape(fieldCount, fieldLength int) ycsbTableShape {
	shape := ycsbTableShape{
		tableType: "BASE TABLE",
		createSQL: "CREATE TABLE `usertable` (`YCSB_KEY` varchar(64) NOT NULL, PRIMARY KEY (`YCSB_KEY`) /*T![clustered_index] CLUSTERED */)",
		indexes:   []ycsbIndexShape{{name: "PRIMARY", column: "YCSB_KEY", sequence: 1, nonUnique: 0}},
		columns:   []ycsbColumnShape{{name: "YCSB_KEY", columnType: "varchar(64)", nullable: "NO", columnKey: "PRI"}},
	}
	for index := 0; index < fieldCount; index++ {
		shape.columns = append(shape.columns, ycsbColumnShape{
			name: fmt.Sprintf("FIELD%d", index), columnType: fmt.Sprintf("varchar(%d)", fieldLength), nullable: "YES",
		})
	}
	return shape
}

func TestValidateYCSBTableShape(t *testing.T) {
	valid := validYCSBTableShape(2, 17)
	if err := validateYCSBTableShape(valid, 2, 17); err != nil {
		t.Fatalf("valid shape rejected: %v", err)
	}
	tests := []struct {
		name      string
		mutate    func(*ycsbTableShape)
		wantError string
	}{
		{name: "partitioned", mutate: func(shape *ycsbTableShape) { shape.partitioned = true }, wantError: "must not be partitioned"},
		{name: "secondary index", mutate: func(shape *ycsbTableShape) {
			shape.indexes = append(shape.indexes, ycsbIndexShape{name: "idx", column: "FIELD0", sequence: 1})
		}, wantError: "exactly the clustered primary key"},
		{name: "wrong field length", mutate: func(shape *ycsbTableShape) { shape.columns[1].columnType = "varchar(18)" }, wantError: "column 1"},
		{name: "not clustered", mutate: func(shape *ycsbTableShape) {
			shape.createSQL = "CREATE TABLE `usertable` (`YCSB_KEY` varchar(64), PRIMARY KEY (`YCSB_KEY`))"
		}, wantError: "not declared CLUSTERED"},
		{name: "cached", mutate: func(shape *ycsbTableShape) { shape.createSQL += " CACHED" }, wantError: "unsupported schema feature"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			shape := validYCSBTableShape(2, 17)
			test.mutate(&shape)
			err := validateYCSBTableShape(shape, 2, 17)
			if err == nil || !strings.Contains(err.Error(), test.wantError) {
				t.Fatalf("got %v, want error containing %q", err, test.wantError)
			}
		})
	}
}

func TestYCSBGatePlanQueriesCoverBindingShapes(t *testing.T) {
	queries, err := ycsbGatePlanQueries("user123", ycsbDefaultFields)
	if err != nil {
		t.Fatal(err)
	}
	wantNames := []string{"point_select", "missing_select", "point_update", "insert_ignore", "range_select", "point_delete"}
	if len(queries) != len(wantNames) {
		t.Fatalf("query count = %d, want %d", len(queries), len(wantNames))
	}
	for index, query := range queries {
		if query.name != wantNames[index] {
			t.Fatalf("query %d name = %q, want %q", index, query.name, wantNames[index])
		}
		if strings.Contains(query.textSQL, "?") {
			t.Fatalf("%s text query contains placeholder: %q", query.name, query.textSQL)
		}
		if !strings.Contains(query.preparedSQL, "?") || len(query.arguments) == 0 {
			t.Fatalf("%s lacks a prepared variant: %q %#v", query.name, query.preparedSQL, query.arguments)
		}
	}
	if !strings.Contains(queries[0].preparedSQL, "FORCE INDEX(`PRIMARY`)") {
		t.Fatalf("point query does not force the primary index: %q", queries[0].preparedSQL)
	}
	if !strings.Contains(queries[4].preparedSQL, "YCSB_KEY >= ? LIMIT ?") {
		t.Fatalf("range query does not match the binding: %q", queries[4].preparedSQL)
	}
}

func TestValidateCheckedSessionVariables(t *testing.T) {
	valid := make(map[string]string, len(expectedCheckedSessionVariables))
	for name, value := range expectedCheckedSessionVariables {
		valid[name] = strings.ToLower(value)
	}
	if err := validateCheckedSessionVariables("test gate", valid); err != nil {
		t.Fatalf("valid session rejected: %v", err)
	}
	delete(valid, "tidb_snapshot")
	if err := validateCheckedSessionVariables("test gate", valid); err == nil || !strings.Contains(err.Error(), "tidb_snapshot is missing") {
		t.Fatalf("missing session variable error = %v", err)
	}
	valid["tidb_snapshot"] = "12345"
	if err := validateCheckedSessionVariables("test gate", valid); err == nil || !strings.Contains(err.Error(), "tidb_snapshot=\"12345\"") {
		t.Fatalf("unexpected session variable error = %v", err)
	}
}

func TestValidateYCSBSessionVariablesRequiresFrozenRetryLimit(t *testing.T) {
	valid := make(map[string]string, len(expectedCheckedSessionVariables)+1)
	for name, value := range expectedCheckedSessionVariables {
		valid[name] = value
	}
	valid["tidb_retry_limit"] = ycsbRetryLimit
	if err := validateYCSBSessionVariables("YCSB test gate", valid); err != nil {
		t.Fatalf("valid YCSB session rejected: %v", err)
	}
	delete(valid, "tidb_retry_limit")
	if err := validateYCSBSessionVariables("YCSB test gate", valid); err == nil || !strings.Contains(err.Error(), "tidb_retry_limit is missing") {
		t.Fatalf("missing retry limit error = %v", err)
	}
	valid["tidb_retry_limit"] = "10"
	if err := validateYCSBSessionVariables("YCSB test gate", valid); err == nil || !strings.Contains(err.Error(), `tidb_retry_limit="10", want "100"`) {
		t.Fatalf("unexpected retry limit error = %v", err)
	}
}

func parseProfile(t *testing.T, path string) map[string]string {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	properties := make(map[string]string)
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 || properties[parts[0]] != "" {
			t.Fatalf("malformed or duplicate property in %s: %q", path, line)
		}
		properties[parts[0]] = parts[1]
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	return properties
}

func TestYCSBProfilesAreFixedAndSemanticOnly(t *testing.T) {
	_, sourceFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("cannot locate test source")
	}
	profileDir := filepath.Join(filepath.Dir(sourceFile), "..", "scripts", "ycsb")
	expectedHashes := map[string]string{
		"workloada.properties": "cb93645ebed3d3cece1133a5874e0741602b2325e190b0eb3af9d0b7bd483a60",
		"workloadb.properties": "3e63b17a5941cc60e835d5cd0ed43d604d280ecd5fb52abfd264c7e0583c2bdb",
		"workloadc.properties": "991e1bb805e85e65d9840a6310e93bd33323640a9d7d4ea1db76f3d1b92990e4",
		"workloadd.properties": "fa1d0bb1956fe30e60c14d10d23375e021cb4a1e0afdd0342f4b35cb22755cfe",
		"workloade.properties": "4d5927eb0e4c45cdafbbbe50e628a0dcc6b948efdc3f79071f30a069ea4174ca",
		"workloadf.properties": "0d89a4c2e8bc741c0bc36815a34380198d365973851d124c05a3b08f205ec663",
	}
	expectedMix := map[string]map[string]string{
		"workloada.properties": {"requestdistribution": "zipfian", "readproportion": "0.5", "updateproportion": "0.5", "insertproportion": "0", "scanproportion": "0", "readmodifywriteproportion": "0"},
		"workloadb.properties": {"requestdistribution": "zipfian", "readproportion": "0.95", "updateproportion": "0.05", "insertproportion": "0", "scanproportion": "0", "readmodifywriteproportion": "0"},
		"workloadc.properties": {"requestdistribution": "zipfian", "readproportion": "1", "updateproportion": "0", "insertproportion": "0", "scanproportion": "0", "readmodifywriteproportion": "0"},
		"workloadd.properties": {"requestdistribution": "latest", "readproportion": "0.95", "updateproportion": "0", "insertproportion": "0.05", "scanproportion": "0", "readmodifywriteproportion": "0"},
		"workloade.properties": {"requestdistribution": "zipfian", "readproportion": "0", "updateproportion": "0", "insertproportion": "0.05", "scanproportion": "0.95", "readmodifywriteproportion": "0"},
		"workloadf.properties": {"requestdistribution": "zipfian", "readproportion": "0.5", "updateproportion": "0", "insertproportion": "0", "scanproportion": "0", "readmodifywriteproportion": "0.5"},
	}
	common := map[string]string{
		"workload": "core", "co-checker.profile": "co-checker-ycsb-core-v1", "fieldcount": "10", "fieldlength": "100",
		"readallfields": "true", "writeallfields": "false", "insertorder": "hashed", "batch.size": "1",
	}
	for name, wantHash := range expectedHashes {
		path := filepath.Join(profileDir, name)
		contents, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		sum := sha256.Sum256(contents)
		if got := hex.EncodeToString(sum[:]); got != wantHash {
			t.Fatalf("%s SHA-256 = %s, want %s", name, got, wantHash)
		}
		properties := parseProfile(t, path)
		for key, want := range common {
			if got := properties[key]; got != want {
				t.Fatalf("%s %s = %q, want %q", name, key, got, want)
			}
		}
		for key, want := range expectedMix[name] {
			if got := properties[key]; got != want {
				t.Fatalf("%s %s = %q, want %q", name, key, got, want)
			}
		}
		for _, forbidden := range []string{"recordcount", "operationcount", "threadcount", "randomseed", "result.path", "mysql.host", "mysql.port", "mysql.database"} {
			if _, exists := properties[forbidden]; exists {
				t.Fatalf("%s contains runner-owned property %s", name, forbidden)
			}
		}
		if name == "workloade.properties" {
			for key, want := range map[string]string{"scanlengthdistribution": "uniform", "minscanlength": "1", "maxscanlength": "100"} {
				if got := properties[key]; got != want {
					t.Fatalf("%s %s = %q, want %q", name, key, got, want)
				}
			}
		}
	}
}

func TestValidateTPCPlanCapabilityGate(t *testing.T) {
	pushedLookupRows := [][]string{
		{"IndexLookUp_1", "1.00", "root"},
		{"IndexRangeScan_2", "1.00", "cop[tikv]"},
		{"TableRowIDScan_3", "0.00", "cop[tikv]"},
	}
	tests := []struct {
		name      string
		operators []string
		rows      [][]string
		mustRead  bool
		wantError string
	}{
		{
			name:      "table reader",
			operators: []string{"TableReader_1", "TableFullScan_2"},
			mustRead:  true,
		},
		{
			name:      "forced pushed index lookup",
			operators: []string{"IndexLookUp_1", "IndexRangeScan_2", "TableRowIDScan_3"},
			rows:      pushedLookupRows,
			mustRead:  true,
		},
		{
			name:      "index lookup without pushed table side",
			operators: []string{"IndexLookUp_1", "IndexRangeScan_2", "TableRowIDScan_3"},
			rows: [][]string{
				{"IndexLookUp_1", "1.00", "root"},
				{"IndexRangeScan_2", "1.00", "cop[tikv]"},
				{"TableRowIDScan_3", "1.00", "cop[tikv]"},
			},
			mustRead:  true,
			wantError: "only 0 pushed table-side scans",
		},
		{
			name:      "unsupported TiFlash",
			operators: []string{"TableReader_1", "TableFullScan_2"},
			rows:      [][]string{{"TableFullScan_2", "1.00", "cop[tiflash]"}},
			mustRead:  true,
			wantError: "unsupported plan row",
		},
		{
			name:      "missing read evidence",
			operators: []string{"Projection_1"},
			mustRead:  true,
			wantError: "no supported point/reader evidence path",
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			err := validateTPCPlan(test.name, test.operators, test.rows, test.mustRead)
			if test.wantError == "" {
				if err != nil {
					t.Fatalf("unexpected error: %v", err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), test.wantError) {
				t.Fatalf("got error %v, want one containing %q", err, test.wantError)
			}
		})
	}
}

func TestTPCCPlanQueriesCoverFullMixAndNewOrderShapes(t *testing.T) {
	queries := tpccPlanQueries()
	requiredPrefixes := []string{
		"new_order_", "payment_", "order_status_", "delivery_", "stock_level_",
	}
	for _, prefix := range requiredPrefixes {
		found := false
		for _, query := range queries {
			if strings.HasPrefix(query.name, prefix) {
				found = true
				break
			}
		}
		if !found {
			t.Fatalf("no plan query covers %s", prefix)
		}
	}

	names := make(map[string]struct{}, len(queries))
	itemShapes := 0
	stockShapes := 0
	insertShapes := 0
	deliveryTupleCoverage := false
	for _, query := range queries {
		if _, exists := names[query.name]; exists {
			t.Fatalf("duplicate plan query name %q", query.name)
		}
		names[query.name] = struct{}{}
		preparedSQL, arguments, err := parameterizeFirstIntegerLiteral(query.sql)
		if err != nil {
			t.Fatalf("parameterize %s: %v", query.name, err)
		}
		if preparedSQL == query.sql || !strings.Contains(preparedSQL, "?") {
			t.Fatalf("%s did not produce a prepared variant: %q", query.name, preparedSQL)
		}
		if len(arguments) != 1 {
			t.Fatalf("%s prepared argument count = %d, want 1", query.name, len(arguments))
		}
		if _, ok := arguments[0].(int64); !ok {
			t.Fatalf("%s prepared argument has type %T, want int64", query.name, arguments[0])
		}
		switch {
		case strings.HasPrefix(query.name, "new_order_select_items_"):
			itemShapes++
		case strings.HasPrefix(query.name, "new_order_select_stock_"):
			stockShapes++
		case strings.HasPrefix(query.name, "new_order_insert_order_line_"):
			insertShapes++
		}
		if query.name == "delivery_select_orders" {
			deliveryTupleCoverage =
				strings.Contains(query.sql, "(1,1,2101)") &&
					strings.Contains(query.sql, "(1,10,2101)") &&
					!strings.Contains(query.sql, "(1,0,")
		}
	}
	if itemShapes != 11 || stockShapes != 11 || insertShapes != 11 {
		t.Fatalf(
			"NewOrder shape counts = items:%d stock:%d inserts:%d, want 11 each",
			itemShapes, stockShapes, insertShapes,
		)
	}
	if !deliveryTupleCoverage {
		t.Fatal("delivery plan gate does not cover districts 1 through 10 with order 2101")
	}
}

func TestParameterizeFirstIntegerLiteralSkipsStringsIdentifiersAndDecimals(t *testing.T) {
	query := "SELECT '123', col_45 FROM t WHERE amount = 1.25 AND id = 7"
	preparedSQL, arguments, err := parameterizeFirstIntegerLiteral(query)
	if err != nil {
		t.Fatal(err)
	}
	if preparedSQL != "SELECT '123', col_45 FROM t WHERE amount = 1.25 AND id = ?" {
		t.Fatalf("prepared SQL = %q", preparedSQL)
	}
	if len(arguments) != 1 {
		t.Fatalf("argument count = %d, want 1", len(arguments))
	}
	if value, ok := arguments[0].(int64); !ok || value != 7 {
		t.Fatalf("argument = %#v, want int64(7)", arguments[0])
	}
}
