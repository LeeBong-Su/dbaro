#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

// AI 도우미 설정: API 키(자격 증명 관리자), 모델, effort, API 주소, 조회 결과 보내기·행 수, 실행 전 묻기.
class AiSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AiSettingsDialog(QWidget *parent = nullptr);

private:
    void save();

    QLineEdit *m_key = nullptr;
    QLabel *m_keyState = nullptr;
    QComboBox *m_model = nullptr;
    QComboBox *m_effort = nullptr;
    QLineEdit *m_baseUrl = nullptr;
    QCheckBox *m_sendRows = nullptr;
    QSpinBox *m_maxRows = nullptr;
    QCheckBox *m_ask = nullptr;
};
