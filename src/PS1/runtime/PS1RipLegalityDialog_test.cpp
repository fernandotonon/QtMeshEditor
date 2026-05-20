#ifdef ENABLE_PS1_RIP
#include <gtest/gtest.h>

#include "PS1/runtime/PS1RipLegalityDialog.h"

#include <QSettings>

TEST(PS1RipLegalityDialogTest, AcknowledgementPersistsInSettings)
{
    QSettings settings;
    settings.remove(QStringLiteral("ps1Rip/acknowledged"));

    EXPECT_FALSE(PS1RipLegalityDialog::isAcknowledged());
    PS1RipLegalityDialog::setAcknowledged(true);
    EXPECT_TRUE(PS1RipLegalityDialog::isAcknowledged());

    settings.remove(QStringLiteral("ps1Rip/acknowledged"));
}
#endif
