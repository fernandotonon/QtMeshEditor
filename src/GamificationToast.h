#ifndef GAMIFICATION_TOAST_H
#define GAMIFICATION_TOAST_H

#include <QVariantList>
#include <QWidget>

class QLabel;
class QTimer;

/// One restrained, dismissible achievement-unlock toast (E-P4 #800).
/// Anchored to the parent window's top-right corner; auto-hides. Multiple
/// unlocks coalesce into a single toast ("… and 2 more") — never more than
/// one toast at a time, no animation (reduced-motion-safe).
class GamificationToast : public QWidget
{
    Q_OBJECT
public:
    /// Shows (or merges into) the toast for @p achievements — a list of maps
    /// with title/xp as delivered by GamificationManager::achievementsUnlocked.
    static void showAchievements(QWidget* parentWindow, const QVariantList& achievements);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    explicit GamificationToast(QWidget* parentWindow);
    void appendAchievements(const QVariantList& achievements);
    void reposition();
    void updateText();

    QLabel* m_label = nullptr;
    QTimer* m_hideTimer = nullptr;
    QStringList m_titles;
    int m_totalXp = 0;
};

#endif  // GAMIFICATION_TOAST_H
