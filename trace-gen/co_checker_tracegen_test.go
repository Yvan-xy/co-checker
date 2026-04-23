// Copyright 2026
//
// This file is copied temporarily into TiDB's pkg/session/test/txn package by
// generate.sh. It intentionally uses TiDB's testkit so the emitted YAML traces
// come from the instrumented TiDB transaction path, not from a synthetic writer.

package txn

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/pingcap/tidb/pkg/testkit"
	"github.com/stretchr/testify/require"
)

func TestCoCheckerTraceGen(t *testing.T) {
	outputDir := os.Getenv("CO_CHECKER_TRACE_OUTPUT_DIR")
	require.NotEmpty(t, outputDir, "set CO_CHECKER_TRACE_OUTPUT_DIR")
	require.NoError(t, os.MkdirAll(outputDir, 0o755))

	runTraceScenario(t, outputDir, "small-basic", generateSmallBasicTrace)
	runTraceScenario(t, outputDir, "small-delete-miss", generateSmallDeleteMissTrace)
	runTraceScenario(t, outputDir, "small-two-session-mutual", generateSmallTwoSessionMutualTrace)
	runTraceScenario(t, outputDir, "medium-mixed", generateMediumMixedTrace)
}

func runTraceScenario(t *testing.T, outputDir, name string, workload func(t *testing.T, tracePath string)) {
	t.Helper()
	t.Run(name, func(t *testing.T) {
		tracePath := filepath.Join(outputDir, name+".yaml")
		require.NoError(t, os.RemoveAll(tracePath))

		workload(t, tracePath)

		data, err := os.ReadFile(tracePath)
		require.NoError(t, err)
		require.NotEmpty(t, data)
		require.Contains(t, string(data), `status: "committed"`)
	})
}

func newTraceKit(t *testing.T) (*testkit.TestKit, *testkit.TestKit, *testkit.TestKit) {
	store := testkit.CreateMockStore(t)

	tk1 := testkit.NewTestKit(t, store)
	tk2 := testkit.NewTestKit(t, store)
	tk3 := testkit.NewTestKit(t, store)
	for _, tk := range []*testkit.TestKit{tk1, tk2, tk3} {
		tk.MustExec("use test")
	}
	return tk1, tk2, tk3
}

func enableTrace(t *testing.T, tracePath string) {
	t.Helper()
	t.Setenv("TIDB_TXN_TRACE_ENABLE", "1")
	t.Setenv("TIDB_TXN_TRACE_PATH", tracePath)
}

func generateSmallBasicTrace(t *testing.T, tracePath string) {
	tk1, _, _ := newTraceKit(t)
	tk1.MustExec("create table accounts(id int primary key, balance int, index idx_balance(balance))")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into accounts values (1, 100)")
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select balance from accounts where id = 1").Check(testkit.Rows("100"))
	tk1.MustExec("update accounts set balance = balance + 10 where id = 1")
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustExec("insert into accounts values (2, 200)")
	tk1.MustExec("rollback")

	tk1.MustExec("begin")
	tk1.MustQuery("select balance from accounts where id = 1").Check(testkit.Rows("110"))
	tk1.MustExec("commit")

	assertTraceContains(t, tracePath, `status: "rolled_back"`)
}

func generateSmallDeleteMissTrace(t *testing.T, tracePath string) {
	tk1, _, _ := newTraceKit(t)
	tk1.MustExec("create table items(id int primary key, qty int)")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into items values (1, 10), (2, 20)")
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select qty from items where id = 99").Check(testkit.Rows())
	tk1.MustExec("delete from items where id = 1")
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select qty from items where id = 1").Check(testkit.Rows())
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustExec("delete from items where id = 2")
	tk1.MustExec("rollback")

	assertTraceContains(t, tracePath, "miss: true")
	assertTraceContains(t, tracePath, "delete: true")
	assertTraceContains(t, tracePath, `status: "rolled_back"`)
}

func generateSmallTwoSessionMutualTrace(t *testing.T, tracePath string) {
	tk1, tk2, _ := newTraceKit(t)
	tk1.MustExec("create table shared_vars(id int primary key, v int)")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into shared_vars values (1, 10), (2, 20)")
	tk1.MustExec("commit")

	session1 := tk1.Session().GetSessionVars().ConnectionID
	session2 := tk2.Session().GetSessionVars().ConnectionID
	require.NotZero(t, session1)
	require.NotZero(t, session2)
	require.NotEqual(t, session1, session2)

	tk1.MustExec("begin")
	tk2.MustExec("begin")

	tk1.MustQuery("select v from shared_vars where id = 1").Check(testkit.Rows("10"))
	tk1.MustQuery("select v from shared_vars where id = 2").Check(testkit.Rows("20"))
	tk2.MustQuery("select v from shared_vars where id = 1").Check(testkit.Rows("10"))
	tk2.MustQuery("select v from shared_vars where id = 2").Check(testkit.Rows("20"))

	tk1.MustExec("update shared_vars set v = v + 1 where id = 1")
	tk2.MustExec("update shared_vars set v = v + 1 where id = 2")

	tk1.MustExec("commit")
	tk2.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select v from shared_vars where id = 1").Check(testkit.Rows("11"))
	tk1.MustQuery("select v from shared_vars where id = 2").Check(testkit.Rows("21"))
	tk1.MustExec("commit")

	assertTraceContains(t, tracePath, fmt.Sprintf("session_id: %d", session1))
	assertTraceContains(t, tracePath, fmt.Sprintf("session_id: %d", session2))
	assertTraceContains(t, tracePath, `source: "snapshot"`)
}

func generateMediumMixedTrace(t *testing.T, tracePath string) {
	tk1, tk2, tk3 := newTraceKit(t)
	tk1.MustExec("create table ledger(id int primary key, v int, index idx_v(v))")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into ledger values " + valueList(1, 10, 10))
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select v from ledger where id = 1").Check(testkit.Rows("10"))
	tk1.MustExec("update ledger set v = 15 where id = 1")
	tk1.MustQuery("select v from ledger where id = 1").Check(testkit.Rows("15"))
	tk1.MustExec("commit")

	tk2.MustExec("begin")
	tk2.MustExec("insert into ledger values (11, 110)")
	tk2.MustHavePlan("select id, v from ledger use index(idx_v)", "UnionScan")
	tk2.MustQuery("select id, v from ledger use index(idx_v) where v between 10 and 110 order by v").Check(
		testkit.Rows("1 15", "2 20", "3 30", "4 40", "5 50", "6 60", "7 70", "8 80", "9 90", "10 100", "11 110"),
	)
	tk2.MustExec("commit")

	tk3.MustExec("begin")
	tk3.MustExec("insert into ledger values (12, 120)")
	tk3.MustQuery("select v from ledger where id = 12").Check(testkit.Rows("120"))
	tk3.MustExec("rollback")

	tk1.MustExec("begin")
	tk1.MustExec("delete from ledger where id = 2")
	tk1.MustExec("commit")

	tk2.MustExec("begin")
	tk2.MustQuery("select v from ledger where id = 2").Check(testkit.Rows())
	tk2.MustQuery("select v from ledger where id = 12").Check(testkit.Rows())
	tk2.MustExec("commit")

	for id := 3; id <= 8; id++ {
		tk1.MustExec("begin")
		tk1.MustQuery(fmt.Sprintf("select v from ledger where id = %d", id)).Check(testkit.Rows(fmt.Sprintf("%d", id*10)))
		tk1.MustExec(fmt.Sprintf("update ledger set v = v + 1 where id = %d", id))
		tk1.MustExec("commit")
	}

	tk3.MustExec("begin")
	tk3.MustExec("insert into ledger values (13, 130), (14, 140)")
	tk3.MustHavePlan("select id, v from ledger use index(idx_v)", "UnionScan")
	tk3.MustQuery("select id, v from ledger use index(idx_v) where id >= 10").Sort().Check(
		testkit.Rows("10 100", "11 110", "13 130", "14 140"),
	)
	tk3.MustExec("rollback")

	tk2.MustExec("begin")
	tk2.MustQuery("select v from ledger where id = 13").Check(testkit.Rows())
	tk2.MustQuery("select v from ledger where id = 14").Check(testkit.Rows())
	tk2.MustExec("commit")

	assertTraceContains(t, tracePath, `status: "rolled_back"`)
	assertTraceContains(t, tracePath, `source: "mem_buffer"`)
	assertTraceContains(t, tracePath, "miss: true")
	assertTraceContains(t, tracePath, "delete: true")
}

func valueList(startID, count, step int) string {
	values := make([]string, 0, count)
	for i := 0; i < count; i++ {
		id := startID + i
		values = append(values, fmt.Sprintf("(%d, %d)", id, id*step))
	}
	return strings.Join(values, ", ")
}

func assertTraceContains(t *testing.T, tracePath, needle string) {
	t.Helper()
	data, err := os.ReadFile(tracePath)
	require.NoError(t, err)
	require.Contains(t, string(data), needle)
}
