package main

import (
	"strings"
	"testing"
)

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
