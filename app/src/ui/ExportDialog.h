#pragma once

#include "core/ResultExport.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;

// 결과 내보내기 설정: 형식, 범위(전체 행 / 선택한 셀), 형식별 옵션, 저장할 곳(파일 / 클립보드).
// 마지막 선택은 기억한다.
class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    ExportDialog(bool hasSelection, bool hasMoreRows, const QString &suggestedTable, SqlDialect dialect,
                 QWidget *parent = nullptr);

    ExportOptions options() const;
    bool selectionOnly() const;
    bool toClipboard() const;
    QString filePath() const;

protected:
    void accept() override;

private:
    void updateForFormat();
    ExportOptions::Format format() const;

    SqlDialect m_dialect;
    QComboBox *m_format = nullptr;
    QRadioButton *m_allRows = nullptr;
    QRadioButton *m_selection = nullptr;
    QCheckBox *m_header = nullptr;
    QCheckBox *m_bom = nullptr;
    QLineEdit *m_nullText = nullptr;
    QLineEdit *m_table = nullptr;
    QLabel *m_nullLabel = nullptr;
    QLabel *m_tableLabel = nullptr;
    QRadioButton *m_toFile = nullptr;
    QRadioButton *m_toClipboard = nullptr;
    QLineEdit *m_path = nullptr;
};
