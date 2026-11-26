// SqlRename: SQL 글 안에서 테이블·컬럼 이름을 찾아 바꾸기
#include "core/SqlRename.h"

#include <QtTest>

namespace {

RenameTarget target(SqlDialect dialect, const QString &schema, const QString &table, const QString &column, const QString &typed)
{
    RenameTarget t;
    t.dialect = dialect;
    t.schema = schema;
    t.table = table;
    t.column = column;
    t.newName = SqlRename::storedName(typed, dialect);
    return t;
}

QList<RenameMatch> suggested(const QList<RenameMatch> &matches, bool value = true)
{
    QList<RenameMatch> list;
    for (const RenameMatch &m : matches) {
        if (m.suggested == value)
            list << m;
    }
    return list;
}

} // namespace

class SqlRenameTest : public QObject
{
    Q_OBJECT

private slots:
    void renamesATableAndKeepsItsLook()
    {
        const QString sql = QStringLiteral(
            "-- emp in a comment stays\n"
            "SELECT e.emp_id, d.dept_name FROM emp e JOIN dept d ON d.dept_id = e.dept_id;\n"
            "SELECT * FROM DBARO.EMP;\n"
            "SELECT * FROM other.emp;\n"
            "UPDATE \"EMP\" SET salary = 0;\n"
            "SELECT emp.emp_id FROM Emp;\n"
            "BEGIN EXECUTE IMMEDIATE 'DELETE FROM emp'; END;\n");
        const auto t = target(SqlDialect::Oracle, QStringLiteral("DBARO"), QStringLiteral("EMP"), {}, QStringLiteral("employee"));
        QCOMPARE(t.newName, QStringLiteral("EMPLOYEE"));
        const QList<RenameMatch> matches = SqlRename::find(sql, t);
        QCOMPARE(suggested(matches).size(), 5);
        QCOMPARE(suggested(matches, false).size(), 1); // 문자열 안
        QCOMPARE(suggested(matches, false).first().line, 7);
        QCOMPARE(SqlRename::apply(sql, suggested(matches)),
                 QStringLiteral("-- emp in a comment stays\n"
                                "SELECT e.emp_id, d.dept_name FROM employee e JOIN dept d ON d.dept_id = e.dept_id;\n"
                                "SELECT * FROM DBARO.EMPLOYEE;\n"
                                "SELECT * FROM other.emp;\n"
                                "UPDATE \"EMPLOYEE\" SET salary = 0;\n"
                                "SELECT employee.emp_id FROM EMPLOYEE;\n"
                                "BEGIN EXECUTE IMMEDIATE 'DELETE FROM emp'; END;\n"));
    }

    void renamesAColumnOnlyWhereTheTableIsUsed()
    {
        const QString sql = QStringLiteral(
            "SELECT e.dept_id, d.dept_id FROM emp e JOIN dept d ON d.dept_id = e.dept_id;\n"
            "SELECT dept_id FROM dept;\n"
            "INSERT INTO emp (emp_id, dept_id) VALUES (1, 10);\n"
            "CREATE OR REPLACE TRIGGER emp_trg BEFORE INSERT ON emp FOR EACH ROW\n"
            "BEGIN\n"
            "  :NEW.dept_id := NVL(:NEW.dept_id, 10);\n"
            "END;\n"
            "/\n"
            "DECLARE v emp.dept_id%TYPE; BEGIN NULL; END;\n"
            "/\n");
        const auto t = target(SqlDialect::Oracle, QStringLiteral("DBARO"), QStringLiteral("EMP"), QStringLiteral("DEPT_ID"),
                              QStringLiteral("department_id"));
        const QList<RenameMatch> matches = SqlRename::find(sql, t);
        QCOMPARE(suggested(matches).size(), 6);       // e.dept_id ×2, INSERT, :NEW ×2, emp.dept_id%TYPE
        QCOMPARE(suggested(matches, false).size(), 2); // d.dept_id ×2
        QVERIFY(suggested(matches, false).first().reason.contains(QLatin1String("d")));
        const QString result = SqlRename::apply(sql, suggested(matches));
        QVERIFY(result.contains(QLatin1String("SELECT e.department_id, d.dept_id FROM emp e")));
        QVERIFY(result.contains(QLatin1String("d.dept_id = e.department_id")));
        QVERIFY(result.contains(QLatin1String("SELECT dept_id FROM dept;"))); // 다른 테이블 문장은 그대로
        QVERIFY(result.contains(QLatin1String("(emp_id, department_id)")));
        QVERIFY(result.contains(QLatin1String(":NEW.department_id := NVL(:NEW.department_id, 10)")));
        QVERIFY(result.contains(QLatin1String("v emp.department_id%TYPE")));
    }

    void looksInsidePostgresFunctionBodies()
    {
        const QString sql = QStringLiteral(
            "CREATE OR REPLACE FUNCTION public.emp_count() RETURNS integer LANGUAGE plpgsql AS $function$\n"
            "BEGIN\n"
            "  RETURN (SELECT count(*) FROM public.emp);\n"
            "END\n"
            "$function$;\n");
        const auto t = target(SqlDialect::PostgreSql, QStringLiteral("public"), QStringLiteral("emp"), {}, QStringLiteral("Staff"));
        QCOMPARE(t.newName, QStringLiteral("staff"));
        const QList<RenameMatch> matches = SqlRename::find(sql, t);
        QCOMPARE(matches.size(), 1); // emp_count는 다른 이름
        QCOMPARE(SqlRename::apply(sql, matches).count(QStringLiteral("FROM public.staff")), 1);
    }

    void keepsMySqlBackticks()
    {
        const QString sql = QStringLiteral("SELECT * FROM `dbaro`.`emp` e WHERE e.`dept_id` = 1 AND e.DEPT_ID > 0");
        const auto t = target(SqlDialect::MySql, QStringLiteral("dbaro"), QStringLiteral("emp"), QStringLiteral("dept_id"),
                              QStringLiteral("department_id"));
        const QList<RenameMatch> matches = SqlRename::find(sql, t);
        QCOMPARE(SqlRename::apply(sql, suggested(matches)),
                 QStringLiteral("SELECT * FROM `dbaro`.`emp` e WHERE e.`department_id` = 1 AND e.DEPARTMENT_ID > 0"));
    }

    void quotesNamesThatNeedIt()
    {
        QCOMPARE(SqlRename::storedName(QStringLiteral("my table"), SqlDialect::Oracle), QStringLiteral("my table"));
        const auto t = target(SqlDialect::Oracle, QStringLiteral("DBARO"), QStringLiteral("EMP"), {}, QStringLiteral("my table"));
        QCOMPARE(SqlRename::apply(QStringLiteral("SELECT * FROM emp"), SqlRename::find(QStringLiteral("SELECT * FROM emp"), t)),
                 QStringLiteral("SELECT * FROM \"my table\""));
        QVERIFY(!SqlRename::validate(QStringLiteral("a\"b")).isEmpty());
        QVERIFY(SqlRename::validate(QStringLiteral("employee")).isEmpty());
    }

    void makesTheRenameStatement()
    {
        QCOMPARE(SqlRename::renameStatement(target(SqlDialect::Oracle, QStringLiteral("DBARO"), QStringLiteral("EMP"), {}, QStringLiteral("employee"))),
                 QStringLiteral("ALTER TABLE DBARO.EMP RENAME TO EMPLOYEE;"));
        QCOMPARE(SqlRename::renameStatement(target(SqlDialect::PostgreSql, QStringLiteral("public"), QStringLiteral("emp"),
                                                   QStringLiteral("dept_id"), QStringLiteral("department_id"))),
                 QStringLiteral("ALTER TABLE public.emp RENAME COLUMN dept_id TO department_id;"));
        QCOMPARE(SqlRename::renameStatement(target(SqlDialect::MySql, QStringLiteral("dbaro"), QStringLiteral("emp"), {}, QStringLiteral("staff"))),
                 QStringLiteral("RENAME TABLE dbaro.emp TO dbaro.staff;"));
    }

    void mentionsOnlyWholeWords()
    {
        QVERIFY(!SqlRename::mentions(QStringLiteral("SELECT emp_id FROM employees"), QStringLiteral("emp")));
        QVERIFY(SqlRename::mentions(QStringLiteral("SELECT * FROM EMP"), QStringLiteral("emp")));
    }
};

QTEST_GUILESS_MAIN(SqlRenameTest)
#include "SqlRenameTest.moc"
