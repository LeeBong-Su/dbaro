#pragma once

#include "core/ConnectionProfile.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QStringList>

class EngineClient;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

// 연결 관리 화면. 왼쪽은 연결 목록, 오른쪽은 선택한 연결의 설정이다.
// 목록의 복사본을 편집하고, 저장을 누르면 호출한 쪽이 한꺼번에 반영한다.
class ConnectionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConnectionDialog(const QList<ConnectionProfile> &profiles, QWidget *parent = nullptr);

    // 연결 테스트에 쓸 엔진. 지정하지 않으면 연결 테스트 버튼이 비활성이다.
    void setEngine(EngineClient *engine);

    void startNewConnection();
    void selectConnection(const QString &id);
    QString selectedId() const;

    QList<ConnectionProfile> profiles() const { return m_profiles; }
    // 저장할 때 OS 보안 저장소에 반영할 비밀번호 (연결 id → 새 비밀번호)
    QHash<QString, QString> pendingPasswords() const { return m_pendingPasswords; }
    QStringList removedIds() const { return m_removedIds; }

public slots:
    void accept() override;

private:
    QWidget *createGeneralPage();
    QWidget *createAdvancedPage();

    void testConnection();
    void addConnection();
    void duplicateConnection();
    void deleteConnection();
    void onCurrentRowChanged(int row);
    void onTypeChanged();
    void onFormEdited();

    void loadForm(const ConnectionProfile &profile);
    void clearForm();
    void storeForm();
    void updateTypeDependentWidgets();
    void updateUrlPreview();
    void refreshListItem(int row);
    void showMessage(const QString &text, bool isError);

    DbType currentType() const;
    QString uniqueName(const QString &base) const;
    bool validate(QString *message, int *badRow) const;

    QList<ConnectionProfile> m_profiles;
    QHash<QString, QString> m_pendingPasswords;
    QStringList m_removedIds;
    int m_currentRow = -1;
    bool m_loading = false;

    QListWidget *m_list = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_duplicateButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QTabWidget *m_pages = nullptr;

    // General
    QFormLayout *m_generalForm = nullptr;
    QLineEdit *m_name = nullptr;
    QComboBox *m_type = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QComboBox *m_oracleTarget = nullptr;
    QLabel *m_databaseLabel = nullptr;
    QLineEdit *m_database = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_savePassword = nullptr;
    QComboBox *m_environment = nullptr;
    QCheckBox *m_readOnly = nullptr;
    QLineEdit *m_urlPreview = nullptr;

    // Advanced
    QSpinBox *m_connectTimeout = nullptr;
    QSpinBox *m_queryTimeout = nullptr;
    QPlainTextEdit *m_initSql = nullptr;
    QLineEdit *m_driverPath = nullptr;
    QLabel *m_driverHint = nullptr;

    QPushButton *m_testButton = nullptr;
    QLabel *m_message = nullptr;
    QPointer<EngineClient> m_engine;
    bool m_testing = false;
};
