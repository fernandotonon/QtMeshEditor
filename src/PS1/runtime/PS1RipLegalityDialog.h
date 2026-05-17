#ifndef PS1RIPLELEGALITYDIALOG_H
#define PS1RIPLELEGALITYDIALOG_H

#include <QDialog>

class QCheckBox;

/** First-run acknowledgement for PS1 runtime ripping (#417). */
class PS1RipLegalityDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PS1RipLegalityDialog(QWidget *parent = nullptr);

    static bool isAcknowledged();
    static void setAcknowledged(bool value);

protected:
    void accept() override;

private:
    QCheckBox *m_ackCheck = nullptr;
};

#endif // PS1RIPLELEGALITYDIALOG_H
