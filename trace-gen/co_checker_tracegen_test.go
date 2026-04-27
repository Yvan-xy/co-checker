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

	runTraceScenario(t, outputDir, "1s-3t-7e-basic", generateSmallBasicTrace)
	runTraceScenario(t, outputDir, "1s-3t-5e-delete-miss", generateSmallDeleteMissTrace)
	runTraceScenario(t, outputDir, "2s-4t-10e-mutual", generateSmallTwoSessionMutualTrace)
	runTraceScenario(t, outputDir, "2s-10t-28e", generate2Session10Transaction28EventTrace)
	runTraceScenario(t, outputDir, "2s-12t-67e-mixed", generateMediumMixedTrace)
	runTraceScenario(t, outputDir, "1s-overwrite-chain", generateOverwriteChainTrace)
	runTraceScenario(t, outputDir, "2s-delete-reinsert", generateDeleteReinsertTrace)
	runTraceScenario(t, outputDir, "3s-fanout", generateThreeSessionFanoutTrace)
	runTraceScenario(t, outputDir, "3s-large-roundrobin", generate3Session30Transaction120EventTrace)
	runTraceScenario(t, outputDir, "3s-large-mixed", generate3Session40Transaction200EventMixedTrace)
	runTraceScenario(t, outputDir, "3s-very-large-roundrobin", generate3SessionVeryLargeRoundRobinTrace)
	runTraceScenario(t, outputDir, "3s-very-large-mixed", generate3SessionVeryLargeMixedTrace)
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

func generate2Session10Transaction28EventTrace(t *testing.T, tracePath string) {
	tk1, tk2, _ := newTraceKit(t)
	tk1.MustExec("create table param_vars(id int primary key, v int)")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into param_vars values (1, 100), (2, 200)")
	tk1.MustExec("commit")

	session1 := tk1.Session().GetSessionVars().ConnectionID
	session2 := tk2.Session().GetSessionVars().ConnectionID
	require.NotZero(t, session1)
	require.NotZero(t, session2)
	require.NotEqual(t, session1, session2)

	v1 := 100
	v2 := 200
	for txnIndex := 0; txnIndex < 8; txnIndex++ {
		tk := tk1
		writeID := 1
		if txnIndex%2 == 1 {
			tk = tk2
			writeID = 2
		}

		tk.MustExec("begin")
		tk.MustQuery("select v from param_vars where id = 1").Check(testkit.Rows(fmt.Sprintf("%d", v1)))
		tk.MustQuery("select v from param_vars where id = 2").Check(testkit.Rows(fmt.Sprintf("%d", v2)))
		tk.MustExec(fmt.Sprintf("update param_vars set v = v + 1 where id = %d", writeID))
		tk.MustExec("commit")

		if writeID == 1 {
			v1++
		} else {
			v2++
		}
	}

	tk2.MustExec("begin")
	tk2.MustQuery("select v from param_vars where id = 1").Check(testkit.Rows(fmt.Sprintf("%d", v1)))
	tk2.MustQuery("select v from param_vars where id = 2").Check(testkit.Rows(fmt.Sprintf("%d", v2)))
	tk2.MustExec("commit")

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

func generateOverwriteChainTrace(t *testing.T, tracePath string) {
	tk1, _, _ := newTraceKit(t)
	tk1.MustExec("create table overwrite_chain(id int primary key, v int, index idx_v(v))")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into overwrite_chain values (1, 10), (2, 20)")
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select v from overwrite_chain where id = 1").Check(testkit.Rows("10"))
	tk1.MustExec("update overwrite_chain set v = 11 where id = 1")
	tk1.MustExec("update overwrite_chain set v = 12 where id = 1")
	tk1.MustQuery("select v from overwrite_chain where id = 1").Check(testkit.Rows("12"))
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select v from overwrite_chain where id = 1").Check(testkit.Rows("12"))
	tk1.MustQuery("select v from overwrite_chain where id = 2").Check(testkit.Rows("20"))
	tk1.MustExec("update overwrite_chain set v = 21 where id = 2")
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select v from overwrite_chain where id = 1").Check(testkit.Rows("12"))
	tk1.MustQuery("select v from overwrite_chain where id = 2").Check(testkit.Rows("21"))
	tk1.MustExec("commit")

	assertTraceContains(t, tracePath, `status: "committed"`)
}

func generateDeleteReinsertTrace(t *testing.T, tracePath string) {
	tk1, tk2, _ := newTraceKit(t)
	tk1.MustExec("create table delete_reinsert(id int primary key, v int)")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into delete_reinsert values (1, 10)")
	tk1.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustExec("delete from delete_reinsert where id = 1")
	tk1.MustExec("commit")

	tk2.MustExec("begin")
	tk2.MustQuery("select v from delete_reinsert where id = 1").Check(testkit.Rows())
	tk2.MustExec("insert into delete_reinsert values (1, 30)")
	tk2.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select v from delete_reinsert where id = 1").Check(testkit.Rows("30"))
	tk1.MustExec("delete from delete_reinsert where id = 1")
	tk1.MustExec("insert into delete_reinsert values (1, 40)")
	tk1.MustExec("commit")

	tk2.MustExec("begin")
	tk2.MustQuery("select v from delete_reinsert where id = 1").Check(testkit.Rows("40"))
	tk2.MustExec("commit")

	assertTraceContains(t, tracePath, "delete: true")
	assertTraceContains(t, tracePath, "miss: true")
}

func generateThreeSessionFanoutTrace(t *testing.T, tracePath string) {
	tk1, tk2, tk3 := newTraceKit(t)
	tk1.MustExec("create table fanout_vars(id int primary key, v int)")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into fanout_vars values (1, 10), (2, 20), (3, 30)")
	tk1.MustExec("commit")

	session1 := tk1.Session().GetSessionVars().ConnectionID
	session2 := tk2.Session().GetSessionVars().ConnectionID
	session3 := tk3.Session().GetSessionVars().ConnectionID
	require.NotZero(t, session1)
	require.NotZero(t, session2)
	require.NotZero(t, session3)
	require.NotEqual(t, session1, session2)
	require.NotEqual(t, session1, session3)
	require.NotEqual(t, session2, session3)

	tk1.MustExec("begin")
	tk2.MustExec("begin")
	tk3.MustExec("begin")

	tk1.MustQuery("select v from fanout_vars where id = 1").Check(testkit.Rows("10"))
	tk1.MustQuery("select v from fanout_vars where id = 2").Check(testkit.Rows("20"))
	tk1.MustQuery("select v from fanout_vars where id = 3").Check(testkit.Rows("30"))
	tk2.MustQuery("select v from fanout_vars where id = 1").Check(testkit.Rows("10"))
	tk2.MustQuery("select v from fanout_vars where id = 2").Check(testkit.Rows("20"))
	tk2.MustQuery("select v from fanout_vars where id = 3").Check(testkit.Rows("30"))
	tk3.MustQuery("select v from fanout_vars where id = 1").Check(testkit.Rows("10"))
	tk3.MustQuery("select v from fanout_vars where id = 2").Check(testkit.Rows("20"))
	tk3.MustQuery("select v from fanout_vars where id = 3").Check(testkit.Rows("30"))

	tk1.MustExec("update fanout_vars set v = 11 where id = 1")
	tk2.MustExec("update fanout_vars set v = 21 where id = 2")
	tk3.MustExec("update fanout_vars set v = 31 where id = 3")

	tk1.MustExec("commit")
	tk2.MustExec("commit")
	tk3.MustExec("commit")

	tk1.MustExec("begin")
	tk1.MustQuery("select v from fanout_vars where id = 1").Check(testkit.Rows("11"))
	tk1.MustQuery("select v from fanout_vars where id = 2").Check(testkit.Rows("21"))
	tk1.MustQuery("select v from fanout_vars where id = 3").Check(testkit.Rows("31"))
	tk1.MustExec("commit")

	assertTraceContains(t, tracePath, fmt.Sprintf("session_id: %d", session1))
	assertTraceContains(t, tracePath, fmt.Sprintf("session_id: %d", session2))
	assertTraceContains(t, tracePath, fmt.Sprintf("session_id: %d", session3))
	assertTraceContains(t, tracePath, `source: "snapshot"`)
}

func generate3Session30Transaction120EventTrace(t *testing.T, tracePath string) {
	tk1, tk2, tk3 := newTraceKit(t)
	kits := []*testkit.TestKit{tk1, tk2, tk3}
	tk1.MustExec("create table roundrobin_vars(id int primary key, v int, index idx_v(v))")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into roundrobin_vars values " + valueList(1, 6, 10))
	tk1.MustExec("commit")

	values := map[int]int{}
	for id := 1; id <= 6; id++ {
		values[id] = id * 10
	}

	for txnIndex := 0; txnIndex < 28; txnIndex++ {
		tk := kits[txnIndex%len(kits)]
		writeID := (txnIndex % 6) + 1
		readA := ((txnIndex + 1) % 6) + 1
		readB := ((txnIndex + 3) % 6) + 1

		tk.MustExec("begin")
		tk.MustQuery(fmt.Sprintf("select v from roundrobin_vars where id = %d", readA)).
			Check(testkit.Rows(fmt.Sprintf("%d", values[readA])))
		tk.MustQuery(fmt.Sprintf("select v from roundrobin_vars where id = %d", readB)).
			Check(testkit.Rows(fmt.Sprintf("%d", values[readB])))
		tk.MustQuery(fmt.Sprintf("select v from roundrobin_vars where id = %d", writeID)).
			Check(testkit.Rows(fmt.Sprintf("%d", values[writeID])))
		tk.MustExec(fmt.Sprintf("update roundrobin_vars set v = v + 1 where id = %d", writeID))
		tk.MustExec("commit")
		values[writeID]++
	}

	tk3.MustExec("begin")
	for id := 1; id <= 6; id++ {
		tk3.MustQuery(fmt.Sprintf("select v from roundrobin_vars where id = %d", id)).
			Check(testkit.Rows(fmt.Sprintf("%d", values[id])))
	}
	tk3.MustExec("commit")

	assertTraceContains(t, tracePath, `source: "snapshot"`)
}

func generate3Session40Transaction200EventMixedTrace(t *testing.T, tracePath string) {
	tk1, tk2, tk3 := newTraceKit(t)
	kits := []*testkit.TestKit{tk1, tk2, tk3}
	tk1.MustExec("create table large_mixed(id int primary key, v int, index idx_v(v))")
	enableTrace(t, tracePath)

	tk1.MustExec("begin")
	tk1.MustExec("insert into large_mixed values " + valueList(1, 20, 5))
	tk1.MustExec("commit")

	values := map[int]int{}
	for id := 1; id <= 20; id++ {
		values[id] = id * 5
	}
	deleted := map[int]bool{}

	for txnIndex := 0; txnIndex < 36; txnIndex++ {
		tk := kits[txnIndex%len(kits)]
		readA := (txnIndex % 20) + 1
		readB := ((txnIndex + 7) % 20) + 1
		writeID := ((txnIndex + 3) % 20) + 1
		missID := 1000 + txnIndex
		pendingValues := copyIntMap(values)
		pendingDeleted := copyBoolMap(deleted)

		tk.MustExec("begin")
		expectPointRead(tk, readA, values, deleted)
		expectPointRead(tk, readB, values, deleted)
		tk.MustQuery(fmt.Sprintf("select v from large_mixed where id = %d", missID)).Check(testkit.Rows())

		if txnIndex%9 == 4 {
			tk.MustExec(fmt.Sprintf("insert into large_mixed values (%d, %d)", missID, missID*2))
		} else if txnIndex%8 == 3 {
			tk.MustExec(fmt.Sprintf("delete from large_mixed where id = %d", writeID))
			pendingDeleted[writeID] = true
		} else {
			if deleted[writeID] {
				pendingValues[writeID] = writeID * 5
				tk.MustExec(fmt.Sprintf("insert into large_mixed values (%d, %d)", writeID, pendingValues[writeID]))
				pendingDeleted[writeID] = false
			} else {
				pendingValues[writeID] += 2
				tk.MustExec(fmt.Sprintf("update large_mixed set v = %d where id = %d", pendingValues[writeID], writeID))
			}
		}

		if txnIndex%10 == 6 {
			tk.MustHavePlan("select id, v from large_mixed use index(idx_v)", "UnionScan")
			tk.MustQuery("select id, v from large_mixed use index(idx_v) where v >= 0 order by id limit 5")
		}

		if txnIndex%13 == 8 {
			tk.MustExec("rollback")
		} else {
			tk.MustExec("commit")
			values = pendingValues
			deleted = pendingDeleted
		}
	}

	tk2.MustExec("begin")
	for id := 1; id <= 5; id++ {
		expectPointRead(tk2, id, values, deleted)
	}
	tk2.MustExec("commit")

	assertTraceContains(t, tracePath, "miss: true")
	assertTraceContains(t, tracePath, "delete: true")
	assertTraceContains(t, tracePath, `status: "rolled_back"`)
}

func generate3SessionVeryLargeRoundRobinTrace(t *testing.T, tracePath string) {
	tk1, tk2, tk3 := newTraceKit(t)
	kits := []*testkit.TestKit{tk1, tk2, tk3}
	tk1.MustExec("create table very_large_roundrobin(id int primary key, v int, index idx_v(v))")
	enableTrace(t, tracePath)

	const keyCount = 60
	const txnCount = 250

	tk1.MustExec("begin")
	tk1.MustExec("insert into very_large_roundrobin values " + valueList(1, keyCount, 10))
	tk1.MustExec("commit")

	values := map[int]int{}
	for id := 1; id <= keyCount; id++ {
		values[id] = id * 10
	}

	for txnIndex := 0; txnIndex < txnCount; txnIndex++ {
		tk := kits[txnIndex%len(kits)]
		writeID := (txnIndex % keyCount) + 1

		tk.MustExec("begin")
		for offset := 0; offset < 5; offset++ {
			readID := ((txnIndex + offset*11) % keyCount) + 1
			tk.MustQuery(fmt.Sprintf("select v from very_large_roundrobin where id = %d", readID)).
				Check(testkit.Rows(fmt.Sprintf("%d", values[readID])))
		}
		tk.MustQuery(fmt.Sprintf("select v from very_large_roundrobin where id = %d", writeID)).
			Check(testkit.Rows(fmt.Sprintf("%d", values[writeID])))
		values[writeID] += (txnIndex % 5) + 1
		tk.MustExec(fmt.Sprintf("update very_large_roundrobin set v = %d where id = %d", values[writeID], writeID))
		tk.MustExec("commit")
	}

	tk3.MustExec("begin")
	for id := 1; id <= keyCount; id++ {
		tk3.MustQuery(fmt.Sprintf("select v from very_large_roundrobin where id = %d", id)).
			Check(testkit.Rows(fmt.Sprintf("%d", values[id])))
	}
	tk3.MustExec("commit")

	assertTraceContains(t, tracePath, `source: "snapshot"`)
}

func generate3SessionVeryLargeMixedTrace(t *testing.T, tracePath string) {
	tk1, tk2, tk3 := newTraceKit(t)
	kits := []*testkit.TestKit{tk1, tk2, tk3}
	tk1.MustExec("create table very_large_mixed(id int primary key, v int, index idx_v(v))")
	enableTrace(t, tracePath)

	const keyCount = 50
	const txnCount = 240

	tk1.MustExec("begin")
	tk1.MustExec("insert into very_large_mixed values " + valueList(1, keyCount, 7))
	tk1.MustExec("commit")

	values := map[int]int{}
	for id := 1; id <= keyCount; id++ {
		values[id] = id * 7
	}
	deleted := map[int]bool{}

	for txnIndex := 0; txnIndex < txnCount; txnIndex++ {
		tk := kits[txnIndex%len(kits)]
		writeID := ((txnIndex * 7) % keyCount) + 1
		missID := 10000 + txnIndex
		pendingValues := copyIntMap(values)
		pendingDeleted := copyBoolMap(deleted)

		tk.MustExec("begin")
		for offset := 0; offset < 4; offset++ {
			readID := ((txnIndex + offset*13) % keyCount) + 1
			expectPointReadFromTable(tk, "very_large_mixed", readID, values, deleted)
		}
		tk.MustQuery(fmt.Sprintf("select v from very_large_mixed where id = %d", missID)).Check(testkit.Rows())

		if txnIndex%17 == 5 {
			tk.MustExec(fmt.Sprintf("insert into very_large_mixed values (%d, %d)", missID, missID*3))
		} else if txnIndex%11 == 4 {
			tk.MustExec(fmt.Sprintf("delete from very_large_mixed where id = %d", writeID))
			pendingDeleted[writeID] = true
		} else {
			if deleted[writeID] {
				pendingValues[writeID] = writeID * 7
				tk.MustExec(fmt.Sprintf("insert into very_large_mixed values (%d, %d)", writeID, pendingValues[writeID]))
				pendingDeleted[writeID] = false
			} else {
				pendingValues[writeID] += (txnIndex % 9) + 1
				tk.MustExec(fmt.Sprintf("update very_large_mixed set v = %d where id = %d", pendingValues[writeID], writeID))
			}
		}

		if txnIndex%20 == 9 {
			tk.MustHavePlan("select id, v from very_large_mixed use index(idx_v)", "UnionScan")
			tk.MustQuery("select id, v from very_large_mixed use index(idx_v) where v >= 0 order by id limit 10")
		}

		if txnIndex%29 == 13 {
			tk.MustExec("rollback")
		} else {
			tk.MustExec("commit")
			values = pendingValues
			deleted = pendingDeleted
		}
	}

	tk2.MustExec("begin")
	for id := 1; id <= keyCount; id++ {
		expectPointReadFromTable(tk2, "very_large_mixed", id, values, deleted)
	}
	tk2.MustExec("commit")

	assertTraceContains(t, tracePath, "miss: true")
	assertTraceContains(t, tracePath, "delete: true")
	assertTraceContains(t, tracePath, `status: "rolled_back"`)
}

func expectPointRead(tk *testkit.TestKit, id int, values map[int]int, deleted map[int]bool) {
	expectPointReadFromTable(tk, "large_mixed", id, values, deleted)
}

func expectPointReadFromTable(tk *testkit.TestKit, table string, id int, values map[int]int, deleted map[int]bool) {
	if deleted[id] {
		tk.MustQuery(fmt.Sprintf("select v from %s where id = %d", table, id)).Check(testkit.Rows())
		return
	}
	tk.MustQuery(fmt.Sprintf("select v from %s where id = %d", table, id)).
		Check(testkit.Rows(fmt.Sprintf("%d", values[id])))
}

func copyIntMap(input map[int]int) map[int]int {
	output := make(map[int]int, len(input))
	for key, value := range input {
		output[key] = value
	}
	return output
}

func copyBoolMap(input map[int]bool) map[int]bool {
	output := make(map[int]bool, len(input))
	for key, value := range input {
		output[key] = value
	}
	return output
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
