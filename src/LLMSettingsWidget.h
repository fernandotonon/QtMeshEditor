#ifndef LLMSETTINGSWIDGET_H
#define LLMSETTINGSWIDGET_H

#include <QDialog>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QListWidget>
#include <QGroupBox>
#include <QTabWidget>
#include "LLMManager.h"
#include "ModelDownloader.h"

class LLMSettingsWidget : public QDialog
{
    Q_OBJECT

public:
    explicit LLMSettingsWidget(QWidget *parent = nullptr);
    ~LLMSettingsWidget() = default;

private slots:
    void onLoadModelClicked();
    void onUnloadModelClicked();
    void onBrowseDirectoryClicked();
    void onRefreshModelsClicked();
    void onDownloadModelClicked();
    void onCancelDownloadClicked();

    void onModelLoadCompleted(const QString &modelName);
    void onModelLoadError(const QString &error);
    void onModelUnloaded();

    void onDownloadProgress(const QString &modelName, qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadCompleted(const QString &modelName, const QString &filePath);
    void onDownloadError(const QString &modelName, const QString &error);

    void onSettingsChanged();
    void onApplySettings();
    void onResetDefaults();

private:
    void setupUI();
    void setupModelsTab(QWidget *parent);
    void setupSettingsTab(QWidget *parent);
    void setupDownloadTab(QWidget *parent);

    void updateModelList();
    void updateRecommendedModelsList();
    void updateStatus();
    void loadCurrentSettings();

    QString formatFileSize(qint64 bytes) const;

private:
    QTabWidget *m_tabWidget;

    // Models tab
    QComboBox *m_modelCombo;
    QPushButton *m_loadButton;
    QPushButton *m_unloadButton;
    QPushButton *m_refreshButton;
    QLineEdit *m_directoryEdit;
    QPushButton *m_browseButton;
    QLabel *m_statusLabel;

    // Settings tab
    QSpinBox *m_contextSizeSpinBox;
    QSpinBox *m_maxTokensSpinBox;
    QDoubleSpinBox *m_temperatureSpinBox;
    QSlider *m_temperatureSlider;
    QSpinBox *m_gpuLayersSpinBox;
    QSpinBox *m_threadsSpinBox;
    QDoubleSpinBox *m_topPSpinBox;
    QSpinBox *m_topKSpinBox;
    QDoubleSpinBox *m_repeatPenaltySpinBox;
    QPushButton *m_applyButton;
    QPushButton *m_resetButton;

    // Download tab
    QListWidget *m_recommendedModelsList;
    QPushButton *m_downloadButton;
    QPushButton *m_cancelDownloadButton;
    QProgressBar *m_downloadProgressBar;
    QLabel *m_downloadStatusLabel;
    QLabel *m_downloadSpeedLabel;
};

#endif // LLMSETTINGSWIDGET_H
