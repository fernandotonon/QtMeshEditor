#ifndef CLOUD_UPLOAD_PROGRESS_H
#define CLOUD_UPLOAD_PROGRESS_H

#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;

class CloudUploadProgress : public QWidget {
    Q_OBJECT

public:
    explicit CloudUploadProgress(QWidget* parent = nullptr);

    void start(const QString& label, int total);
    void updateProgress(int current, int total, const QString& fileName);
    void finish(bool ok, const QString& message);
    void hideProgress();

signals:
    void cancelRequested();

private:
    QLabel* m_label = nullptr;
    QProgressBar* m_bar = nullptr;
    QPushButton* m_cancelButton = nullptr;
};

#endif // CLOUD_UPLOAD_PROGRESS_H
