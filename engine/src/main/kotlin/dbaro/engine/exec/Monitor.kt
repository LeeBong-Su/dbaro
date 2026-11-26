package dbaro.engine.exec

import java.math.BigDecimal
import java.sql.Connection
import java.sql.ResultSet
import java.sql.SQLException
import java.sql.Types
import java.time.format.DateTimeFormatter

/**
 * 모니터링: 세션, 락(누가 누구를 막는지), 용량(테이블스페이스·DB·큰 객체), 세션 종료.
 * DBMS마다 다른 시스템 뷰를 읽어 앱이 같은 모양으로 그릴 수 있게 맞춘다.
 *
 * - Oracle: V$SESSION, V$SQLAREA, V$LOCK, V$LOCKED_OBJECT, DBA_* (SELECT_CATALOG_ROLE 또는 SELECT ANY DICTIONARY 필요)
 * - Tibero: Oracle과 같은 이름의 뷰. 컬럼이 다를 수 있어 있는 컬럼만 쓴다 (실제 DB로 확인하지 못함)
 * - PostgreSQL: pg_stat_activity, pg_blocking_pids, pg_locks, pg_database_size, pg_total_relation_size
 * - MySQL: processlist, performance_schema.data_lock_waits·data_locks, information_schema.INNODB_TRX·TABLES·FILES
 * - MariaDB: processlist, information_schema.INNODB_LOCK_WAITS·INNODB_LOCKS·INNODB_TRX·TABLES
 *
 * 세션 id는 종료할 때 쓰는 값이다: Oracle·Tibero "SID,SERIAL#", PostgreSQL pid, MySQL·MariaDB 연결 id.
 * 권한이 없어 일부만 읽은 부분은 errors(구역 → 글)와 notes로 알린다.
 */
object Monitor {
    private const val MAX_ROWS = 5_000
    private const val TOP_OBJECTS = 50
    private const val QUERY_TIMEOUT_SEC = 30
    private val SECONDS = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
    private val SESSION_ID = Regex("""^\d+,\d+$""")
    private val NUMBER_ID = Regex("""^\d+$""")

    private fun oracleFamily(type: String) = type == "oracle" || type == "tibero"
    private fun mysqlFamily(type: String) = type == "mysql" || type == "mariadb"

    // ---------------------------------------------------------------------
    // 세션

    fun sessions(type: String, c: Connection): Map<String, Any?> {
        val notes = ArrayList<String>()
        val sessions = try {
            when {
                type == "oracle" -> oracleSessions(c)
                type == "tibero" -> tiberoSessions(c)
                type == "postgres" -> postgresSessions(c)
                mysqlFamily(type) -> mysqlSessions(type, c, notes)
                else -> throw SQLException("Session monitoring is not supported for $type.")
            }
        } catch (e: SQLException) {
            throw explain(type, e)
        }
        return mapOf("sessions" to sessions, "notes" to notes)
    }

    private fun oracleSessions(c: Connection): List<Map<String, Any?>> {
        val list = rows(
            c,
            """
        SELECT s.sid, s.serial# AS serial, s.username, s.status, s.type, s.osuser, s.machine, s.program, s.module,
               s.schemaname, s.event, s.wait_class, s.state, s.last_call_et, s.logon_time,
               s.blocking_session, s.sql_id, s.prev_sql_id,
               (SELECT q.sql_text FROM v${'$'}sqlarea q WHERE q.sql_id = NVL(s.sql_id, s.prev_sql_id)) AS sql_text,
               (SELECT ROUND((SYSDATE - t.start_date) * 86400) FROM v${'$'}transaction t WHERE t.addr = s.taddr) AS xact_seconds,
               CASE WHEN s.sid = SYS_CONTEXT('USERENV', 'SID') THEN 1 ELSE 0 END AS self
          FROM v${'$'}session s
            """,
        )
        // BLOCKING_SESSION은 SID만 준다: 같은 목록에서 SERIAL#을 찾아 종료할 때 쓰는 id로 맞춘다
        val ids = list.associate { it.str("sid") to "${it.str("sid")},${it.str("serial")}" }
        return list.map { r -> oracleSession(r, ids) }
    }

    private fun oracleSession(r: Map<String, Any?>, ids: Map<String?, String>): Map<String, Any?> {
        val background = r.str("type") == "BACKGROUND"
        val status = r.str("status")
        val waiting = r.str("state") == "WAITING"
        val blocker = r.str("blocking_session")
        return session(
            id = "${r.str("sid")},${r.str("serial")}",
            user = r.str("username"),
            status = status,
            active = status == "ACTIVE" && !background,
            seconds = r.long("last_call_et"),
            wait = if (status == "ACTIVE" && !waiting) "ON CPU" else r.str("event"),
            idleWait = r.str("wait_class") == "Idle",
            blockedBy = listOfNotNull(blocker?.let { ids[it] ?: it }),
            database = r.str("schemaname"),
            client = r.str("machine"),
            osUser = r.str("osuser"),
            program = r.str("program"),
            module = r.str("module"),
            sql = r.str("sql_text"),
            sqlId = r.str("sql_id") ?: r.str("prev_sql_id"),
            sqlIsPrevious = r.str("sql_id") == null && r.str("prev_sql_id") != null,
            logonTime = r.str("logon_time"),
            transactionSeconds = r.long("xact_seconds"),
            background = background,
            self = r.long("self") == 1L,
        )
    }

    /** Tibero V$SESSION은 버전마다 컬럼이 달라서 있는 컬럼만 고른다. */
    private fun tiberoSessions(c: Connection): List<Map<String, Any?>> {
        val self = runCatching { scalar(c, "SELECT SYS_CONTEXT('USERENV', 'SID') FROM DUAL") }.getOrNull()
        return rows(c, "SELECT * FROM V${'$'}SESSION").map { r ->
            val sid = r.pick("sid")
            val type = r.pick("type")
            val background = type != null && type != "USER" && type != "WTHR"
            val status = r.pick("status")
            session(
                id = "$sid,${r.pick("serial#", "serial")}",
                user = r.pick("username", "user_name"),
                status = status,
                active = status.equals("RUNNING", true) || status.equals("ACTIVE", true),
                seconds = r.pick("last_call_et", "elapsed_time")?.toLongOrNull(),
                wait = r.pick("wait_event", "event"),
                idleWait = false,
                blockedBy = emptyList(),
                database = r.pick("schemaname", "schema_name"),
                client = r.pick("machine", "ipaddr", "client_ip"),
                osUser = r.pick("osuser", "os_user"),
                program = r.pick("prog_name", "program"),
                module = r.pick("module"),
                sql = null,
                sqlId = r.pick("sql_id"),
                sqlIsPrevious = false,
                logonTime = r.pick("logon_time"),
                transactionSeconds = null,
                background = background,
                self = sid != null && sid == self,
            )
        }
    }

    private fun postgresSessions(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT a.pid, a.usename, a.datname, a.application_name, a.client_addr::text AS client_addr, a.client_hostname,
               a.backend_start, a.state, a.wait_event_type, a.wait_event, a.backend_type, a.query,
               array_to_string(pg_blocking_pids(a.pid), ',') AS blocked_by,
               EXTRACT(EPOCH FROM (now() - CASE WHEN a.state = 'active' THEN a.query_start ELSE a.state_change END))::bigint AS seconds,
               EXTRACT(EPOCH FROM (now() - a.xact_start))::bigint AS xact_seconds,
               a.pid = pg_backend_pid() AS self
          FROM pg_stat_activity a
        """,
    ).map { r ->
        val backendType = r.str("backend_type")
        val background = backendType != null && backendType != "client backend"
        val state = r.str("state")
        val waitType = r.str("wait_event_type")
        val wait = r.str("wait_event")
        session(
            id = r.str("pid")!!,
            user = r.str("usename"),
            status = state ?: backendType,
            active = !background && state != null && state != "idle",
            seconds = r.long("seconds"),
            wait = if (waitType != null && wait != null) "$waitType: $wait" else null,
            idleWait = waitType == "Client" || waitType == "Activity",
            blockedBy = r.str("blocked_by")?.split(',')?.filter { it.isNotEmpty() }.orEmpty(),
            database = r.str("datname"),
            client = r.str("client_hostname") ?: r.str("client_addr") ?: if (background) null else "local",
            osUser = null,
            program = r.str("application_name")?.takeIf { it.isNotEmpty() } ?: if (background) backendType else null,
            module = null,
            sql = r.str("query")?.takeIf { it.isNotEmpty() },
            sqlId = null,
            sqlIsPrevious = state != null && state != "active",
            logonTime = r.str("backend_start"),
            transactionSeconds = r.long("xact_seconds"),
            background = background,
            self = r.bool("self"),
        )
    }

    private fun mysqlSessions(type: String, c: Connection, notes: MutableList<String>): List<Map<String, Any?>> {
        val self = scalar(c, "SELECT CONNECTION_ID()")
        // MySQL 8.0.22+는 performance_schema.processlist가 권장 (information_schema 쪽은 사라질 예정)
        val list = (if (type == "mysql") runCatching { processlist(c, "performance_schema.processlist") }.getOrNull() else null)
            ?: processlist(c, "information_schema.PROCESSLIST")

        // 열린 트랜잭션 시간과 락 대기는 PROCESS 권한이 있어야 읽힌다 (없으면 자기 세션만 보인다)
        val hasProcess = runCatching {
            scalar(c, "SELECT COUNT(*) FROM information_schema.USER_PRIVILEGES WHERE PRIVILEGE_TYPE = 'PROCESS'")
        }.getOrNull()?.toLongOrNull()?.let { it > 0 } ?: true
        val transactions = HashMap<String, Long?>()
        val blockers = HashMap<String, MutableList<String>>()
        if (hasProcess) {
            runCatching {
                rows(c, "SELECT trx_mysql_thread_id AS id, TIMESTAMPDIFF(SECOND, trx_started, NOW()) AS secs FROM information_schema.INNODB_TRX")
                    .forEach { transactions[it.str("id")!!] = it.long("secs") }
            }.onFailure { notes.add("Open transactions are not shown: ${it.message}") }
            runCatching { mysqlWaits(type, c) }.getOrNull()?.forEach { w ->
                blockers.getOrPut(w.str("waiter_id")!!) { ArrayList() }.add(w.str("blocker_id")!!)
            }
        } else {
            notes.add("Only your own sessions are shown, without transactions and lock waits. The PROCESS privilege shows every session.")
        }

        return list.map { r ->
            val id = r.str("id")!!
            val user = r.str("user")
            val command = r.str("command")
            val background = command in setOf("Daemon", "Binlog Dump", "Binlog Dump GTID") ||
                user == "event_scheduler" || user == "system user"
            session(
                id = id,
                user = user,
                status = command,
                active = !background && command != "Sleep",
                seconds = r.long("time"),
                wait = r.str("state")?.takeIf { it.isNotEmpty() },
                idleWait = command == "Sleep",
                blockedBy = blockers[id].orEmpty(),
                database = r.str("db"),
                client = r.str("host"),
                osUser = null,
                program = null,
                module = null,
                sql = r.str("info"),
                sqlId = null,
                sqlIsPrevious = false,
                logonTime = null,
                transactionSeconds = transactions[id],
                background = background,
                self = id == self,
            )
        }
    }

    private fun processlist(c: Connection, view: String) =
        rows(c, "SELECT ID AS id, USER AS user, HOST AS host, DB AS db, COMMAND AS command, TIME AS time, STATE AS state, INFO AS info FROM $view")

    private fun session(
        id: String, user: String?, status: String?, active: Boolean, seconds: Long?, wait: String?, idleWait: Boolean,
        blockedBy: List<String>, database: String?, client: String?, osUser: String?, program: String?, module: String?,
        sql: String?, sqlId: String?, sqlIsPrevious: Boolean, logonTime: String?, transactionSeconds: Long?,
        background: Boolean, self: Boolean,
    ): Map<String, Any?> = mapOf(
        "id" to id, "user" to user, "status" to status, "active" to active, "seconds" to seconds, "wait" to wait,
        "idleWait" to idleWait, "blockedBy" to blockedBy, "database" to database, "client" to client, "osUser" to osUser,
        "program" to program, "module" to module, "sql" to sql, "sqlId" to sqlId, "sqlIsPrevious" to sqlIsPrevious,
        "logonTime" to logonTime, "transactionSeconds" to transactionSeconds, "background" to background, "self" to self,
    )

    // ---------------------------------------------------------------------
    // 락

    /**
     * waits: 락을 기다리는 세션과 막고 있는 세션 (한 줄이 한 쌍)
     * held: 지금 잡혀 있는 락 (Oracle 잠긴 객체, PostgreSQL AccessShareLock보다 센 테이블 락,
     *       MySQL 테이블 락, MariaDB 락을 잡은 트랜잭션)
     */
    fun locks(type: String, c: Connection): Map<String, Any?> {
        val errors = LinkedHashMap<String, String>()
        val waits = section("waits", errors, type) {
            when {
                type == "oracle" -> oracleWaits(c)
                type == "tibero" -> tiberoWaits(c)
                type == "postgres" -> postgresWaits(c)
                mysqlFamily(type) -> mysqlWaits(type, c).map { normalizeWait(it) }
                else -> emptyList()
            }
        }
        val held = section("held", errors, type) {
            when {
                oracleFamily(type) -> oracleHeld(c)
                type == "postgres" -> postgresHeld(c)
                type == "mysql" -> mysqlHeld(c)
                type == "mariadb" -> mariadbHeld(c)
                else -> emptyList()
            }
        }
        return mapOf("waits" to waits, "held" to held, "errors" to errors)
    }

    private fun oracleWaits(c: Connection): List<Map<String, Any?>> {
        val objects = oracleObjectsView(c)
        return rows(
            c,
            """
            SELECT w.sid || ',' || w.serial# AS waiter_id, w.username AS waiter_user, w.seconds_in_wait AS waiter_seconds,
                   w.event AS wait_event,
                   (SELECT q.sql_text FROM v${'$'}sqlarea q WHERE q.sql_id = w.sql_id) AS waiter_sql,
                   b.sid || ',' || b.serial# AS blocker_id, b.username AS blocker_user, b.status AS blocker_status,
                   b.last_call_et AS blocker_seconds,
                   (SELECT q.sql_text FROM v${'$'}sqlarea q WHERE q.sql_id = NVL(b.sql_id, b.prev_sql_id)) AS blocker_sql,
                   (SELECT o.owner || '.' || o.object_name FROM $objects o WHERE o.object_id = w.row_wait_obj#) AS object,
                   (SELECT MAX(l.type || ':' || l.request) FROM v${'$'}lock l WHERE l.sid = w.sid AND l.request > 0) AS lock_request
              FROM v${'$'}session w
              JOIN v${'$'}session b ON b.sid = w.blocking_session
             WHERE w.blocking_session IS NOT NULL
             ORDER BY w.seconds_in_wait DESC
            """,
        ).map { r ->
            normalizeWait(r + ("lock_mode" to r.str("lock_request")?.let { oracleLock(it) }))
        }
    }

    private fun tiberoWaits(c: Connection): List<Map<String, Any?>> {
        // Tibero는 V$SESSION에 막는 세션 컬럼이 없을 수 있다: V$LOCK의 대기(REQUEST)·보유(LMODE)로 짝짓는다.
        return rows(
            c,
            """
            SELECT w.sid AS waiter_sid, h.sid AS blocker_sid, w.type AS lock_type, w.request AS lock_request
              FROM V${'$'}LOCK w JOIN V${'$'}LOCK h ON h.id1 = w.id1 AND h.id2 = w.id2 AND h.lmode > 0 AND h.sid <> w.sid
             WHERE w.request > 0
            """,
        ).map { r ->
            mapOf(
                "waiterId" to r.str("waiter_sid"), "waiterUser" to null, "waiterSeconds" to null, "waiterSql" to null,
                "waitEvent" to null, "lock" to "${r.str("lock_type")}:${r.str("lock_request")}", "object" to null,
                "blockerId" to r.str("blocker_sid"), "blockerUser" to null, "blockerStatus" to null,
                "blockerSeconds" to null, "blockerSql" to null,
            )
        }
    }

    private fun postgresWaits(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT w.pid AS waiter_id, w.usename AS waiter_user,
               EXTRACT(EPOCH FROM (now() - w.state_change))::bigint AS waiter_seconds,
               w.wait_event_type || ': ' || w.wait_event AS wait_event, w.query AS waiter_sql,
               b.pid AS blocker_id, b.usename AS blocker_user, b.state AS blocker_status,
               EXTRACT(EPOCH FROM (now() - COALESCE(b.xact_start, b.state_change)))::bigint AS blocker_seconds,
               b.query AS blocker_sql,
               -- 행 락은 transactionid를 기다리지만, 그 행의 테이블은 같이 잡은 tuple 락에 있다
               (SELECT l.relation::regclass::text FROM pg_locks l
                 WHERE l.pid = w.pid AND l.relation IS NOT NULL AND (NOT l.granted OR l.locktype = 'tuple')
                 ORDER BY l.granted LIMIT 1) AS object,
               (SELECT l.locktype || ' ' || l.mode FROM pg_locks l WHERE l.pid = w.pid AND NOT l.granted LIMIT 1) AS lock_mode
          FROM pg_stat_activity w
          CROSS JOIN LATERAL unnest(pg_blocking_pids(w.pid)) AS bp(pid)
          JOIN pg_stat_activity b ON b.pid = bp.pid
         ORDER BY waiter_seconds DESC NULLS LAST
        """,
    ).map { normalizeWait(it) }

    /** MySQL 8: performance_schema.data_lock_waits, MariaDB: information_schema.INNODB_LOCK_WAITS */
    private fun mysqlWaits(type: String, c: Connection): List<Map<String, Any?>> {
        val common = """
            r.trx_mysql_thread_id AS waiter_id, pr.USER AS waiter_user,
            TIMESTAMPDIFF(SECOND, r.trx_wait_started, NOW()) AS waiter_seconds, r.trx_query AS waiter_sql,
            pr.STATE AS wait_event,
            b.trx_mysql_thread_id AS blocker_id, pb.USER AS blocker_user, pb.COMMAND AS blocker_status,
            TIMESTAMPDIFF(SECOND, b.trx_started, NOW()) AS blocker_seconds, COALESCE(b.trx_query, pb.INFO) AS blocker_sql
        """
        val joins = """
            JOIN information_schema.INNODB_TRX r ON r.trx_id = %s
            JOIN information_schema.INNODB_TRX b ON b.trx_id = %s
            LEFT JOIN information_schema.PROCESSLIST pr ON pr.ID = r.trx_mysql_thread_id
            LEFT JOIN information_schema.PROCESSLIST pb ON pb.ID = b.trx_mysql_thread_id
        """
        val sql = if (type == "mysql") {
            """
            SELECT $common, CONCAT(dl.OBJECT_SCHEMA, '.', dl.OBJECT_NAME) AS object, CONCAT(dl.LOCK_TYPE, ' ', dl.LOCK_MODE) AS lock_mode
              FROM performance_schema.data_lock_waits w
              ${joins.format("w.REQUESTING_ENGINE_TRANSACTION_ID", "w.BLOCKING_ENGINE_TRANSACTION_ID")}
              LEFT JOIN performance_schema.data_locks dl ON dl.ENGINE_LOCK_ID = w.REQUESTING_ENGINE_LOCK_ID
             ORDER BY waiter_seconds DESC
            """
        } else {
            """
            SELECT $common, REPLACE(l.lock_table, '`', '') AS object, CONCAT(l.lock_type, ' ', l.lock_mode) AS lock_mode
              FROM information_schema.INNODB_LOCK_WAITS w
              ${joins.format("w.requesting_trx_id", "w.blocking_trx_id")}
              LEFT JOIN information_schema.INNODB_LOCKS l ON l.lock_id = w.requested_lock_id
             ORDER BY waiter_seconds DESC
            """
        }
        return rows(c, sql)
    }

    private fun normalizeWait(r: Map<String, Any?>): Map<String, Any?> = mapOf(
        "waiterId" to r.str("waiter_id"), "waiterUser" to r.str("waiter_user"), "waiterSeconds" to r.long("waiter_seconds"),
        "waiterSql" to r.str("waiter_sql"), "waitEvent" to r.str("wait_event"), "lock" to r.str("lock_mode"),
        "object" to r.str("object"), "blockerId" to r.str("blocker_id"), "blockerUser" to r.str("blocker_user"),
        "blockerStatus" to r.str("blocker_status"), "blockerSeconds" to r.long("blocker_seconds"),
        "blockerSql" to r.str("blocker_sql"),
    )

    private fun oracleHeld(c: Connection): List<Map<String, Any?>> {
        val objects = oracleObjectsView(c)
        return rows(
            c,
            """
            SELECT lo.session_id || ',' || s.serial# AS id, NVL(s.username, lo.oracle_username) AS username,
                   o.owner || '.' || o.object_name AS object, o.object_type, lo.locked_mode, s.status, s.last_call_et
              FROM v${'$'}locked_object lo
              JOIN v${'$'}session s ON s.sid = lo.session_id
              LEFT JOIN $objects o ON o.object_id = lo.object_id
             ORDER BY s.last_call_et DESC
            """,
        ).map { r ->
            held(r.str("id"), r.str("username"), r.str("object"), oracleLockMode(r.long("locked_mode")),
                r.str("status"), r.long("last_call_et"), r.str("object_type"))
        }
    }

    private fun postgresHeld(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT l.pid AS id, a.usename, l.relation::regclass::text AS object, l.mode, l.granted, a.state,
               EXTRACT(EPOCH FROM (now() - COALESCE(a.xact_start, a.state_change)))::bigint AS seconds
          FROM pg_locks l
          JOIN pg_stat_activity a ON a.pid = l.pid
          JOIN pg_class c ON c.oid = l.relation
         WHERE l.locktype = 'relation' AND l.relation >= 16384 AND l.mode <> 'AccessShareLock'
           AND l.database = (SELECT oid FROM pg_database WHERE datname = current_database())
           AND c.relkind IN ('r', 'p', 'm', 'v')
           AND l.pid <> pg_backend_pid()
         ORDER BY seconds DESC NULLS LAST
        """,
    ).map { r ->
        held(r.str("id"), r.str("usename"), r.str("object"), r.str("mode"),
            if (r.bool("granted")) r.str("state") else "waiting", r.long("seconds"), null)
    }

    private fun mysqlHeld(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT t.PROCESSLIST_ID AS id, t.PROCESSLIST_USER AS username, CONCAT(dl.OBJECT_SCHEMA, '.', dl.OBJECT_NAME) AS object,
               dl.LOCK_MODE AS mode, dl.LOCK_STATUS AS status, TIMESTAMPDIFF(SECOND, x.trx_started, NOW()) AS seconds
          FROM performance_schema.data_locks dl
          JOIN performance_schema.threads t ON t.THREAD_ID = dl.THREAD_ID
          LEFT JOIN information_schema.INNODB_TRX x ON x.trx_mysql_thread_id = t.PROCESSLIST_ID
         WHERE dl.LOCK_TYPE = 'TABLE'
         ORDER BY seconds DESC
        """,
    ).map { r ->
        held(r.str("id"), r.str("username"), r.str("object"), r.str("mode"), r.str("status"), r.long("seconds"), null)
    }

    /** MariaDB는 잡힌 락 전체를 보여 주는 뷰가 없어 락을 잡은 트랜잭션을 보인다. */
    private fun mariadbHeld(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT x.trx_mysql_thread_id AS id, p.USER AS username, x.trx_state AS status,
               TIMESTAMPDIFF(SECOND, x.trx_started, NOW()) AS seconds, x.trx_rows_locked, x.trx_tables_locked
          FROM information_schema.INNODB_TRX x
          LEFT JOIN information_schema.PROCESSLIST p ON p.ID = x.trx_mysql_thread_id
         WHERE x.trx_rows_locked > 0 OR x.trx_tables_locked > 0
         ORDER BY seconds DESC
        """,
    ).map { r ->
        held(r.str("id"), r.str("username"), null, "transaction", r.str("status"), r.long("seconds"),
            "rows locked ${r.str("trx_rows_locked")}, tables ${r.str("trx_tables_locked")}")
    }

    private fun held(id: String?, user: String?, objectName: String?, mode: String?, status: String?, seconds: Long?, detail: String?) =
        mapOf("id" to id, "user" to user, "object" to objectName, "mode" to mode, "status" to status,
            "seconds" to seconds, "detail" to detail)

    private fun oracleLock(typeAndRequest: String): String {
        val (lockType, request) = typeAndRequest.split(':', limit = 2).let { it[0] to it.getOrNull(1)?.toLongOrNull() }
        return "$lockType ${oracleLockMode(request) ?: request}"
    }

    private fun oracleLockMode(mode: Long?): String? = when (mode) {
        0L -> "NONE"
        1L -> "NULL"
        2L -> "ROW SHARE"
        3L -> "ROW EXCLUSIVE"
        4L -> "SHARE"
        5L -> "SHARE ROW EXCLUSIVE"
        6L -> "EXCLUSIVE"
        else -> mode?.toString()
    }

    /** DBA_OBJECTS를 읽을 수 있으면 그것을, 아니면 ALL_OBJECTS (내가 볼 수 있는 객체만) */
    private fun oracleObjectsView(c: Connection): String =
        if (runCatching { scalar(c, "SELECT 1 FROM dba_objects WHERE ROWNUM = 1") }.isSuccess) "dba_objects" else "all_objects"

    // ---------------------------------------------------------------------
    // 용량

    /**
     * tablespaces: 테이블스페이스(MySQL은 InnoDB 공용 테이블스페이스). 할당(allocatedBytes)·사용·여유·최대(자동 확장)
     * databases: DB별 크기 (Oracle은 DB 하나: 데이터 파일 + 임시 파일 + 리두 로그)
     * objects: 큰 테이블·인덱스 (Oracle 세그먼트, PostgreSQL 지금 DB의 테이블, MySQL 테이블)
     */
    fun space(type: String, c: Connection): Map<String, Any?> {
        val errors = LinkedHashMap<String, String>()
        val tablespaces = section("tablespaces", errors, type) {
            when {
                oracleFamily(type) -> oracleTablespaces(c)
                type == "postgres" -> postgresTablespaces(c)
                type == "mysql" -> mysqlTablespaces(c)
                else -> emptyList()
            }
        }
        val databases = section("databases", errors, type) {
            when {
                oracleFamily(type) -> oracleDatabase(c)
                type == "postgres" -> postgresDatabases(c)
                mysqlFamily(type) -> mysqlDatabases(type, c)
                else -> emptyList()
            }
        }
        val objects = section("objects", errors, type) {
            when {
                oracleFamily(type) -> oracleSegments(c)
                type == "postgres" -> postgresRelations(c)
                mysqlFamily(type) -> mysqlTables(c)
                else -> emptyList()
            }
        }
        return mapOf("tablespaces" to tablespaces, "databases" to databases, "objects" to objects, "errors" to errors)
    }

    private fun oracleTablespaces(c: Connection): List<Map<String, Any?>> {
        fun sql(tempFree: Boolean) = """
            SELECT t.tablespace_name AS name, t.contents AS type, t.status,
                   NVL(d.bytes, 0) AS allocated, NVL(d.maxbytes, 0) AS max_bytes, d.files, d.autoext,
                   ${if (tempFree) "CASE WHEN t.contents = 'TEMPORARY' THEN tf.free_space ELSE NVL(f.bytes, 0) END" else "NVL(f.bytes, 0)"} AS free_bytes
              FROM dba_tablespaces t
              LEFT JOIN (SELECT tablespace_name, SUM(bytes) AS bytes,
                                SUM(CASE WHEN autoextensible = 'YES' THEN GREATEST(maxbytes, bytes) ELSE bytes END) AS maxbytes,
                                COUNT(*) AS files, MAX(autoextensible) AS autoext
                           FROM dba_data_files GROUP BY tablespace_name
                         UNION ALL
                         SELECT tablespace_name, SUM(bytes),
                                SUM(CASE WHEN autoextensible = 'YES' THEN GREATEST(maxbytes, bytes) ELSE bytes END),
                                COUNT(*), MAX(autoextensible)
                           FROM dba_temp_files GROUP BY tablespace_name) d ON d.tablespace_name = t.tablespace_name
              LEFT JOIN (SELECT tablespace_name, SUM(bytes) AS bytes FROM dba_free_space GROUP BY tablespace_name) f
                ON f.tablespace_name = t.tablespace_name
              ${if (tempFree) "LEFT JOIN dba_temp_free_space tf ON tf.tablespace_name = t.tablespace_name" else ""}
             ORDER BY t.tablespace_name
        """
        // DBA_TEMP_FREE_SPACE가 없는 DB(Tibero 등)는 임시 테이블스페이스 여유를 모른다
        val list = runCatching { rows(c, sql(true)) }.getOrElse { rows(c, sql(false)) }
        return list.map { r ->
            val allocated = r.long("allocated") ?: 0L
            val free = r.long("free_bytes")
            tablespace(
                name = r.str("name"), type = r.str("type"), status = r.str("status"),
                allocated = allocated, used = free?.let { (allocated - it).coerceAtLeast(0) }, free = free,
                max = r.long("max_bytes"), autoextend = r.str("autoext") == "YES", files = r.long("files"), location = null,
            )
        }
    }

    private fun postgresTablespaces(c: Connection): List<Map<String, Any?>> =
        rows(c, "SELECT oid, spcname, pg_tablespace_location(oid) AS location FROM pg_tablespace ORDER BY spcname").map { r ->
            // 권한이 없는 테이블스페이스는 크기를 못 읽는다 (CREATE 권한 또는 pg_read_all_stats)
            val size = runCatching { scalar(c, "SELECT pg_tablespace_size(${r.str("oid")}::oid)")?.toLongOrNull() }.getOrNull()
            tablespace(
                name = r.str("spcname"), type = null, status = null, allocated = size, used = size, free = null,
                max = null, autoextend = false, files = null, location = r.str("location")?.takeIf { it.isNotEmpty() },
            )
        }

    /** MySQL 8: 공용 테이블스페이스만 (system, undo, temporary, 일반). 테이블별 .ibd는 objects 쪽에서 본다. */
    private fun mysqlTablespaces(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT TABLESPACE_NAME AS name, FILE_TYPE AS type, STATUS AS status, COUNT(*) AS files,
               SUM(TOTAL_EXTENTS * EXTENT_SIZE) AS allocated, SUM(DATA_FREE) AS free_bytes,
               SUM(MAXIMUM_SIZE) AS max_bytes, MAX(AUTOEXTEND_SIZE) AS autoext, MIN(FILE_NAME) AS location
          FROM information_schema.FILES
         WHERE ENGINE = 'InnoDB' AND TABLESPACE_NAME NOT LIKE '%/%'
         GROUP BY TABLESPACE_NAME, FILE_TYPE, STATUS
         ORDER BY TABLESPACE_NAME
        """,
    ).map { r ->
        val allocated = r.long("allocated")
        val free = r.long("free_bytes")
        tablespace(
            name = r.str("name"), type = r.str("type"), status = r.str("status"), allocated = allocated,
            used = if (allocated != null && free != null) (allocated - free).coerceAtLeast(0) else null, free = free,
            max = r.long("max_bytes")?.takeIf { it > 0 }, autoextend = (r.long("autoext") ?: 0) > 0,
            files = r.long("files"), location = r.str("location"),
        )
    }

    private fun tablespace(
        name: String?, type: String?, status: String?, allocated: Long?, used: Long?, free: Long?, max: Long?,
        autoextend: Boolean, files: Long?, location: String?,
    ) = mapOf(
        "name" to name, "type" to type, "status" to status, "allocatedBytes" to allocated, "usedBytes" to used,
        "freeBytes" to free, "maxBytes" to max, "autoextend" to autoextend, "files" to files, "location" to location,
    )

    private fun oracleDatabase(c: Connection): List<Map<String, Any?>> {
        val name = runCatching { scalar(c, "SELECT SYS_CONTEXT('USERENV', 'DB_NAME') FROM DUAL") }.getOrNull()
        val data = scalar(c, "SELECT NVL(SUM(bytes), 0) FROM dba_data_files")?.toLongOrNull() ?: 0L
        val temp = runCatching { scalar(c, "SELECT NVL(SUM(bytes), 0) FROM dba_temp_files")?.toLongOrNull() }.getOrNull() ?: 0L
        val redo = runCatching { scalar(c, "SELECT NVL(SUM(bytes * members), 0) FROM v${'$'}log")?.toLongOrNull() }.getOrNull() ?: 0L
        val used = runCatching { scalar(c, "SELECT NVL(SUM(bytes), 0) FROM dba_segments")?.toLongOrNull() }.getOrNull()
        return listOf(
            mapOf(
                "name" to name, "bytes" to data + temp + redo, "usedBytes" to used, "dataBytes" to data,
                "tempBytes" to temp, "redoBytes" to redo,
            ),
        )
    }

    private fun postgresDatabases(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT d.datname AS name,
               CASE WHEN has_database_privilege(d.datname, 'CONNECT') THEN pg_database_size(d.datname) END AS bytes,
               d.datname = current_database() AS current
          FROM pg_database d
         WHERE NOT d.datistemplate
         ORDER BY bytes DESC NULLS LAST
        """,
    ).map { r -> mapOf("name" to r.str("name"), "bytes" to r.long("bytes"), "current" to r.bool("current")) }

    private fun mysqlDatabases(type: String, c: Connection): List<Map<String, Any?>> {
        freshTableStats(type, c)
        return rows(
            c,
            """
            SELECT TABLE_SCHEMA AS name, COUNT(*) AS tables, SUM(DATA_LENGTH) AS data_bytes, SUM(INDEX_LENGTH) AS index_bytes,
                   SUM(DATA_FREE) AS free_bytes, SUM(DATA_LENGTH + INDEX_LENGTH) AS bytes
              FROM information_schema.TABLES
             WHERE TABLE_TYPE = 'BASE TABLE' AND COALESCE(ENGINE, '') <> 'PERFORMANCE_SCHEMA'
             GROUP BY TABLE_SCHEMA
             ORDER BY bytes DESC
            """,
        ).map { r ->
            mapOf(
                "name" to r.str("name"), "bytes" to r.long("bytes"), "dataBytes" to r.long("data_bytes"),
                "indexBytes" to r.long("index_bytes"), "freeBytes" to r.long("free_bytes"), "objects" to r.long("tables"),
                "current" to false,
            )
        }
    }

    private fun oracleSegments(c: Connection): List<Map<String, Any?>> {
        fun sql(view: String, owner: String) = """
            SELECT * FROM (
                SELECT $owner AS schema_name, segment_name AS name, MIN(segment_type) AS type,
                       MIN(tablespace_name) AS tablespace, SUM(bytes) AS total_bytes, COUNT(*) AS parts
                  FROM $view
                 GROUP BY $owner, segment_name
                 ORDER BY total_bytes DESC
            ) WHERE ROWNUM <= $TOP_OBJECTS
        """
        // DBA_SEGMENTS를 못 읽으면 내 세그먼트만
        val list = runCatching { rows(c, sql("dba_segments", "owner")) }
            .getOrElse { rows(c, sql("user_segments", "USER")) }
        return list.map { r ->
            obj(r.str("schema_name"), r.str("name"), r.str("type"), r.long("total_bytes"), null, null, null,
                r.str("tablespace"), r.long("parts")?.takeIf { it > 1 })
        }
    }

    private fun postgresRelations(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT n.nspname AS schema_name, c.relname AS name,
               CASE c.relkind WHEN 'r' THEN 'TABLE' WHEN 'p' THEN 'PARTITIONED TABLE' WHEN 'm' THEN 'MATERIALIZED VIEW' END AS type,
               pg_total_relation_size(c.oid) AS bytes, pg_relation_size(c.oid) AS data_bytes, pg_indexes_size(c.oid) AS index_bytes,
               GREATEST(c.reltuples, 0)::bigint AS row_estimate, t.spcname AS tablespace
          FROM pg_class c
          JOIN pg_namespace n ON n.oid = c.relnamespace
          LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace
         WHERE c.relkind IN ('r', 'p', 'm')
           AND n.nspname NOT IN ('pg_catalog', 'information_schema') AND n.nspname NOT LIKE 'pg_toast%'
         ORDER BY pg_total_relation_size(c.oid) DESC
         LIMIT $TOP_OBJECTS
        """,
    ).map { r ->
        obj(r.str("schema_name"), r.str("name"), r.str("type"), r.long("bytes"), r.long("data_bytes"),
            r.long("index_bytes"), r.long("row_estimate"), r.str("tablespace"), null)
    }

    private fun mysqlTables(c: Connection): List<Map<String, Any?>> = rows(
        c,
        """
        SELECT TABLE_SCHEMA AS schema_name, TABLE_NAME AS name, ENGINE AS type, DATA_LENGTH + INDEX_LENGTH AS bytes,
               DATA_LENGTH AS data_bytes, INDEX_LENGTH AS index_bytes, TABLE_ROWS AS row_estimate
          FROM information_schema.TABLES
         WHERE TABLE_TYPE = 'BASE TABLE' AND COALESCE(ENGINE, '') <> 'PERFORMANCE_SCHEMA' -- 메모리에만 있는 표
         ORDER BY DATA_LENGTH + INDEX_LENGTH DESC
         LIMIT $TOP_OBJECTS
        """,
    ).map { r ->
        obj(r.str("schema_name"), r.str("name"), r.str("type"), r.long("bytes"), r.long("data_bytes"),
            r.long("index_bytes"), r.long("row_estimate"), null, null)
    }

    /** MySQL 8은 information_schema.TABLES 통계를 하루까지 캐시한다: 이 세션만 바로 읽게 한다. */
    private fun freshTableStats(type: String, c: Connection) {
        if (type == "mysql") runCatching { c.createStatement().use { it.execute("SET SESSION information_schema_stats_expiry = 0") } }
    }

    private fun obj(
        schema: String?, name: String?, type: String?, bytes: Long?, data: Long?, index: Long?, rows: Long?,
        tablespace: String?, parts: Long?,
    ) = mapOf(
        "schema" to schema, "name" to name, "type" to type, "bytes" to bytes, "dataBytes" to data, "indexBytes" to index,
        "rows" to rows, "tablespace" to tablespace, "parts" to parts,
    )

    // ---------------------------------------------------------------------
    // 세션 종료

    /**
     * 세션을 끊거나(kill) 실행 중인 문장만 멈춘다(cancelQuery). 실행한 문장을 돌려준다.
     * Oracle `ALTER SYSTEM KILL SESSION 'sid,serial#' IMMEDIATE` / `ALTER SYSTEM CANCEL SQL` (18c+),
     * Tibero `ALTER SYSTEM KILL SESSION (sid, serial#)`, PostgreSQL `pg_terminate_backend` / `pg_cancel_backend`,
     * MySQL·MariaDB `KILL CONNECTION` / `KILL QUERY`.
     */
    fun kill(type: String, c: Connection, id: String, cancelQuery: Boolean): Map<String, Any?> {
        val sql = when {
            oracleFamily(type) -> {
                require(SESSION_ID.matches(id)) { "Invalid session id: $id" }
                when {
                    type == "tibero" && cancelQuery -> throw SQLException("Tibero cannot cancel only the running statement. Kill the session instead.")
                    type == "tibero" -> "ALTER SYSTEM KILL SESSION (${id.replace(",", ", ")})"
                    cancelQuery -> "ALTER SYSTEM CANCEL SQL '$id'"
                    else -> "ALTER SYSTEM KILL SESSION '$id' IMMEDIATE"
                }
            }
            type == "postgres" -> {
                require(NUMBER_ID.matches(id)) { "Invalid process id: $id" }
                if (cancelQuery) "SELECT pg_cancel_backend($id)" else "SELECT pg_terminate_backend($id)"
            }
            mysqlFamily(type) -> {
                require(NUMBER_ID.matches(id)) { "Invalid connection id: $id" }
                if (cancelQuery) "KILL QUERY $id" else "KILL CONNECTION $id"
            }
            else -> throw SQLException("Killing sessions is not supported for $type.")
        }
        try {
            if (type == "postgres") {
                val signalled = scalar(c, sql)
                if (signalled != "t" && signalled != "true") {
                    throw SQLException("PostgreSQL did not signal process $id. It may have ended, or you need superuser or pg_signal_backend.")
                }
            } else {
                c.createStatement().use { it.execute(sql) }
            }
        } catch (e: SQLException) {
            throw explain(type, e)
        }
        return mapOf("sql" to sql)
    }

    // ---------------------------------------------------------------------
    // 도우미

    private fun <T> section(name: String, errors: MutableMap<String, String>, type: String, work: () -> List<T>): List<T> =
        try {
            work()
        } catch (e: SQLException) {
            errors[name] = explain(type, e).message ?: e.toString()
            emptyList()
        }

    /** 권한이 없어서 난 오류에 무엇이 필요한지 덧붙인다. */
    private fun explain(type: String, e: SQLException): SQLException {
        val message = e.message ?: return e
        val hint = when {
            oracleFamily(type) && (message.contains("ORA-00942") || message.contains("ORA-01031") || message.contains("TBR-8033")) ->
                "Needs SELECT_CATALOG_ROLE or SELECT ANY DICTIONARY (and ALTER SYSTEM to kill sessions)."
            mysqlFamily(type) && (message.contains("PROCESS privilege") || message.contains("Access denied")) ->
                "Needs the PROCESS privilege (CONNECTION_ADMIN or SUPER to kill other users' sessions)."
            type == "postgres" && message.contains("permission denied") ->
                "Needs superuser, pg_read_all_stats or pg_signal_backend."
            else -> return e
        }
        return SQLException("$message\n$hint", e.sqlState, e.errorCode, e)
    }

    private fun rows(c: Connection, sql: String): List<Map<String, Any?>> =
        c.createStatement().use { st ->
            st.queryTimeout = QUERY_TIMEOUT_SEC
            st.maxRows = MAX_ROWS
            st.executeQuery(sql.trimIndent()).use { rs ->
                val meta = rs.metaData
                val labels = (1..meta.columnCount).map { meta.getColumnLabel(it).lowercase() }
                val types = IntArray(meta.columnCount) { meta.getColumnType(it + 1) }
                val list = ArrayList<Map<String, Any?>>()
                while (rs.next()) {
                    val row = HashMap<String, Any?>(labels.size * 2)
                    for (i in labels.indices) row[labels[i]] = value(rs, i + 1, types[i])
                    list.add(row)
                }
                list
            }
        }

    private fun scalar(c: Connection, sql: String): String? =
        c.createStatement().use { st -> st.executeQuery(sql).use { rs -> if (rs.next()) rs.getString(1) else null } }

    private fun value(rs: ResultSet, i: Int, type: Int): Any? = try {
        when (type) {
            Types.TINYINT, Types.SMALLINT, Types.INTEGER, Types.BIGINT, Types.NUMERIC, Types.DECIMAL,
            Types.REAL, Types.FLOAT, Types.DOUBLE -> rs.getBigDecimal(i)?.let { number(it) }
            Types.BOOLEAN, Types.BIT -> rs.getObject(i)?.let { it == true || it.toString() == "1" || it.toString() == "t" }
            Types.DATE, Types.TIMESTAMP, Types.TIMESTAMP_WITH_TIMEZONE ->
                rs.getTimestamp(i)?.toLocalDateTime()?.format(SECONDS)
            Types.CLOB, Types.NCLOB -> rs.getClob(i)?.let { it.getSubString(1, minOf(it.length(), 65_536L).toInt()) }
            else -> rs.getString(i)
        }
    } catch (e: SQLException) {
        rs.getString(i)
    }

    private fun number(value: BigDecimal): Any =
        if (value.scale() <= 0 || value.stripTrailingZeros().scale() <= 0) value.toLong() else value.toDouble()

    private fun Map<String, Any?>.str(key: String): String? = this[key]?.toString()

    private fun Map<String, Any?>.long(key: String): Long? = when (val v = this[key]) {
        null -> null
        is Number -> v.toLong()
        else -> v.toString().toBigDecimalOrNull()?.toLong()
    }

    private fun Map<String, Any?>.bool(key: String): Boolean = when (val v = this[key]) {
        is Boolean -> v
        is Number -> v.toInt() != 0
        null -> false
        else -> v.toString() == "t" || v.toString().equals("true", true)
    }

    /** 처음 있는 컬럼 값 (Tibero처럼 컬럼 이름이 버전마다 다른 뷰) */
    private fun Map<String, Any?>.pick(vararg keys: String): String? = keys.firstNotNullOfOrNull { this[it]?.toString() }
}
