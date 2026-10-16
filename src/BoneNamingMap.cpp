/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "BoneNamingMap.h"

#include "AppSettingsKeys.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

namespace BoneNaming {

namespace {

/// Case-transfer helper: produce `replacement` cased like `sample`.
/// "L" → "R", "l" → "r", "Left" → "Right", "LEFT" → "RIGHT".
QString matchCase(const QString& replacement, const QString& sample)
{
    if (sample.isEmpty()) return replacement;
    // All-caps sample (and longer than one char, so "L" doesn't force "RIGHT").
    const bool allUpper = sample.size() > 1
        && sample == sample.toUpper()
        && sample != sample.toLower();
    if (allUpper) return replacement.toUpper();
    if (sample.at(0).isUpper())
        return replacement.left(1).toUpper() + replacement.mid(1).toLower();
    return replacement.toLower();
}

/// A side word pair as it appears in a name, with the index/length of the
/// matched token so we can splice a cased replacement back in.
struct WordHit {
    int index = -1;
    int length = 0;
    Side side = Side::Center;
};

/// Find "left"/"right" as a WORD inside `name`, case-insensitively.
///
/// "Word" means the token is not glued to further letters that would make it a
/// different word: `LeftHand` is a hit (next char is uppercase — a new word in
/// camelCase), `hand_left` is a hit (end of string), but `Lefty` is NOT (the
/// trailing lowercase 'y' continues the word). Without that rule a bone called
/// "Lefty" or "Rightmost" would be silently rewritten.
WordHit findSideWord(const QString& name)
{
    struct Cand { const char* word; Side side; };
    static const Cand kCands[] = {
        {"right", Side::Right},  // checked first: "right" contains no "left",
        {"left",  Side::Left},   // but ordering keeps intent obvious
    };
    WordHit best;
    for (const auto& c : kCands) {
        const QString w = QString::fromLatin1(c.word);
        int from = 0;
        while (true) {
            const int at = name.indexOf(w, from, Qt::CaseInsensitive);
            if (at < 0) break;
            from = at + 1;
            const int end = at + w.size();
            // Reject when the next character continues the same word
            // (a lowercase letter). Uppercase = camelCase boundary; digits and
            // separators = boundary; end of string = boundary.
            if (end < name.size()) {
                const QChar next = name.at(end);
                if (next.isLetter() && next.isLower()) continue;
            }
            // Reject when the PRECEDING character is a lowercase letter that
            // would make this the tail of a longer word ("upleft" is fine as a
            // side token, but "shoplefting" style glue is not a real case;
            // the common real case is a separator or camelCase boundary).
            if (at > 0) {
                const QChar prev = name.at(at - 1);
                if (prev.isLetter() && prev.isLower() && !name.at(at).isUpper())
                    continue;
            }
            if (best.index < 0 || at < best.index) {
                best.index = at;
                best.length = w.size();
                best.side = c.side;
            }
        }
    }
    return best;
}

/// Separator characters that delimit a side affix.
bool isSep(QChar c)
{
    return c == QLatin1Char('_') || c == QLatin1Char('.')
        || c == QLatin1Char('-') || c == QLatin1Char(' ')
        || c == QLatin1Char(':');
}

Side sideFromLetter(QChar c)
{
    const QChar lc = c.toLower();
    if (lc == QLatin1Char('l')) return Side::Left;
    if (lc == QLatin1Char('r')) return Side::Right;
    return Side::Center;
}

QChar flipLetter(QChar c)
{
    const bool upper = c.isUpper();
    const QChar lc = c.toLower();
    QChar out = lc;
    if (lc == QLatin1Char('l')) out = QLatin1Char('r');
    else if (lc == QLatin1Char('r')) out = QLatin1Char('l');
    return upper ? out.toUpper() : out;
}

} // namespace

// ---------------------------------------------------------------------------

bool CustomRule::isValid() const
{
    if (leftPattern.isEmpty() || rightPattern.isEmpty()) return false;
    return QRegularExpression(leftPattern).isValid()
        && QRegularExpression(rightPattern).isValid();
}

QString conventionId(Convention c)
{
    switch (c) {
    case Convention::UnderscoreLR:   return QStringLiteral("underscore_lr");
    case Convention::DotLR:          return QStringLiteral("dot_lr");
    case Convention::LeftRightWord:  return QStringLiteral("left_right_word");
    case Convention::SuffixLeftWord: return QStringLiteral("suffix_left_word");
    case Convention::AffixLetter:    return QStringLiteral("affix_letter");
    case Convention::BipedToken:     return QStringLiteral("biped_token");
    case Convention::Custom:         return QStringLiteral("custom");
    case Convention::None:           break;
    }
    return QStringLiteral("none");
}

// ---------------------------------------------------------------------------

QString Map::applyRules(const QString& boneName, Convention* matched, Side* side) const
{
    auto finish = [&](const QString& out, Convention c, Side s) {
        if (matched) *matched = c;
        if (side) *side = s;
        return out;
    };
    if (matched) *matched = Convention::None;
    if (side) *side = Side::Center;
    if (boneName.isEmpty()) return {};

    // --- 1. custom rules first, so a user can override a built-in reading ---
    for (const CustomRule& rule : m_custom) {
        if (!rule.isValid()) continue;
        const QRegularExpression lre(rule.leftPattern);
        if (lre.match(boneName).hasMatch()) {
            QString out = boneName;
            out.replace(lre, rule.leftReplacement);
            if (out != boneName) return finish(out, Convention::Custom, Side::Left);
        }
        const QRegularExpression rre(rule.rightPattern);
        if (rre.match(boneName).hasMatch()) {
            QString out = boneName;
            out.replace(rre, rule.rightReplacement);
            if (out != boneName) return finish(out, Convention::Custom, Side::Right);
        }
    }

    // --- 2. separator + single letter, at either end ------------------------
    // Hand_l / Hand.R / Hand-l  and  L_Hand / R.Hand.
    //
    // Checked BEFORE the "left"/"right" word rule on purpose. A name can carry
    // both signals — Mixamo-derived rigs produce things like
    // "mixamorig_LeftUpLeg_l", where "Left" is part of the inherited bone NAME
    // and the "_l" suffix is the actual side marker. Flipping the word there
    // yields "mixamorig_RightUpLeg_l": a name whose two halves disagree about
    // the side, and which matches no bone in the skeleton. The explicit
    // delimited affix is the more specific signal, so it wins.
    if (boneName.size() >= 2) {
        const QChar last = boneName.at(boneName.size() - 1);
        const Side lastSide = sideFromLetter(last);
        if (lastSide != Side::Center && isSep(boneName.at(boneName.size() - 2))) {
            QString out = boneName;
            out[out.size() - 1] = flipLetter(last);
            const bool dot = boneName.at(boneName.size() - 2) == QLatin1Char('.');
            return finish(out, dot ? Convention::DotLR : Convention::UnderscoreLR, lastSide);
        }
        const QChar first = boneName.at(0);
        const Side firstSide = sideFromLetter(first);
        if (firstSide != Side::Center && isSep(boneName.at(1))) {
            QString out = boneName;
            out[0] = flipLetter(first);
            const bool dot = boneName.at(1) == QLatin1Char('.');
            return finish(out, dot ? Convention::DotLR : Convention::UnderscoreLR, firstSide);
        }
    }

    // --- 3. "left"/"right" as a word, anywhere in the name -----------------
    // Covers Mixamo / Unity HumanIK prefixes (LeftHand), suffix words
    // (hand_left), and namespaced names (mixamorig:LeftArm) in one rule,
    // since the token is located by search rather than by position.
    if (const WordHit hit = findSideWord(boneName); hit.index >= 0) {
        const QString token = boneName.mid(hit.index, hit.length);
        const QString other = (hit.side == Side::Left)
            ? QStringLiteral("right") : QStringLiteral("left");
        QString out = boneName;
        out.replace(hit.index, hit.length, matchCase(other, token));
        // A trailing side word ("hand_left") is reported as its own convention
        // so the diagnostic panel can tell the user which style it detected.
        const bool trailing = (hit.index + hit.length == boneName.size());
        return finish(out,
                      trailing ? Convention::SuffixLeftWord : Convention::LeftRightWord,
                      hit.side);
    }

    // --- 4. 3ds Max Biped standalone token: "Bip001 L UpperArm" ------------
    // The side is a whole token between separators. Checked before the bare
    // camelCase affix so "Bip001 L Thigh" isn't read as a stray letter.
    {
        static const QRegularExpression tokRe(
            QStringLiteral("(^|[ _\\-.:])([LlRr])([ _\\-.:])"));
        const QRegularExpressionMatch m = tokRe.match(boneName);
        if (m.hasMatch()) {
            const QChar letter = m.captured(2).at(0);
            const Side s = sideFromLetter(letter);
            QString out = boneName;
            out[m.capturedStart(2)] = flipLetter(letter);
            return finish(out, Convention::BipedToken, s);
        }
    }

    // --- 5. bare camelCase / glued single letter ---------------------------
    // lHand / rHand (leading letter followed by an uppercase word) and
    // HandL / HandR (trailing uppercase letter after a lowercase run).
    // Deliberately the LAST rule and deliberately strict: a bare trailing
    // 'l'/'r' with no case signal is far more often a real word ending
    // ("Shoulder", "Heel") than a side token, so those are left as centre
    // bones rather than guessed at.
    if (boneName.size() >= 2) {
        const QChar first = boneName.at(0);
        const Side firstSide = sideFromLetter(first);
        if (firstSide != Side::Center && first.isLower() && boneName.at(1).isUpper()) {
            QString out = boneName;
            out[0] = flipLetter(first);
            return finish(out, Convention::AffixLetter, firstSide);
        }
        const QChar last = boneName.at(boneName.size() - 1);
        const Side lastSide = sideFromLetter(last);
        if (lastSide != Side::Center && last.isUpper()
            && boneName.at(boneName.size() - 2).isLower()) {
            QString out = boneName;
            out[out.size() - 1] = flipLetter(last);
            return finish(out, Convention::AffixLetter, lastSide);
        }
    }

    return finish({}, Convention::None, Side::Center);
}

Side Map::sideOf(const QString& boneName) const
{
    Side s = Side::Center;
    applyRules(boneName, nullptr, &s);
    return s;
}

QString Map::counterpart(const QString& boneName) const
{
    return applyRules(boneName, nullptr, nullptr);
}

Convention Map::conventionOf(const QString& boneName) const
{
    Convention c = Convention::None;
    applyRules(boneName, &c, nullptr);
    return c;
}

PairingReport Map::pair(const QStringList& boneNames) const
{
    PairingReport rep;
    const QSet<QString> present(boneNames.begin(), boneNames.end());

    // Track which flipped-name targets have been claimed so a collision
    // (two bones flipping onto the same name) is reported, not silently
    // dropped the way a bare hash insert would.
    QHash<QString, QString> claimedBy; // target name → source bone
    QSet<QString> consumed;            // bones already listed inside a pair

    // PRE-PASS: count how many present bones aim at each target, so a
    // collision is reported independently of the order the bones happen to
    // arrive in. Detecting it inline during the main loop made the diagnostic
    // order-dependent: with the non-mutual source first it failed the
    // mutuality check before claiming the target, the mutual pair then
    // succeeded, and the same rig reported no collision at all.
    QHash<QString, int> aimedAt;
    for (const QString& name : boneNames) {
        Side s = Side::Center;
        const QString t = applyRules(name, nullptr, &s);
        if (t.isEmpty() || s == Side::Center || !present.contains(t)) continue;
        ++aimedAt[t];
    }
    for (auto it = aimedAt.constBegin(); it != aimedAt.constEnd(); ++it)
        if (it.value() > 1) rep.collisions.push_back(it.key());
    // Stable output: QHash iteration order is unspecified.
    rep.collisions.sort();

    for (const QString& name : boneNames) {
        Convention conv = Convention::None;
        Side side = Side::Center;
        const QString other = applyRules(name, &conv, &side);

        if (other.isEmpty() || side == Side::Center) {
            rep.centerBones.push_back(name);
            continue;
        }
        if (!present.contains(other)) {
            rep.unpairedSided.push_back(name);
            continue;
        }
        if (consumed.contains(name)) continue; // already emitted as the partner

        if (claimedBy.constFind(other) != claimedBy.constEnd()) {
            // `other` was already paired off with a different bone. The
            // pre-pass has already recorded the collision; just don't emit a
            // second pair for the same target.
            continue;
        }
        // A pair must be MUTUAL. A custom rule can map A -> B without B -> A;
        // reporting that as a pair makes `buildPairTable` send both sides'
        // weights to B instead of swapping them — corrupting a mirror while
        // reporting success. Treat a one-way mapping as unpaired instead.
        if (counterpart(other) != name) {
            rep.unpairedSided.push_back(name);
            continue;
        }
        claimedBy.insert(other, name);
        consumed.insert(name);
        consumed.insert(other);

        // Emit with the LEFT bone first regardless of which side we hit
        // first, so the panel reads consistently.
        if (side == Side::Left) rep.pairs.push_back({name, other, conventionId(conv)});
        else                    rep.pairs.push_back({other, name, conventionId(conv)});
    }
    return rep;
}

std::vector<int> buildPairTable(const QStringList& boneNamesByHandle,
                                const Map& naming,
                                int* pairCount)
{
    const int n = boneNamesByHandle.size();
    std::vector<int> table(static_cast<size_t>(std::max(0, n)), -1);
    for (int h = 0; h < n; ++h) {
        const QString other = naming.counterpart(boneNamesByHandle.at(h));
        if (other.isEmpty()) continue;                   // centre bone
        const int oh = boneNamesByHandle.indexOf(other);
        if (oh < 0 || oh >= n) continue;                 // partner not present
        // Only a MUTUAL mapping is a swap (see Map::pair) — a one-way custom
        // rule would fold both sides onto one bone.
        if (naming.counterpart(other) != boneNamesByHandle.at(h)) continue;
        table[static_cast<size_t>(h)] = oh;
    }
    // Counted via pair() rather than halving the mapped-bone count: an
    // asymmetric custom rule can map A→B without B→A, which would make the
    // naive /2 round a real pair away. pair() is the one definition of a pair.
    if (pairCount) *pairCount = naming.pair(boneNamesByHandle).pairCount();
    return table;
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kKeyLeftPattern = "leftPattern";
constexpr const char* kKeyLeftReplacement = "leftReplacement";
constexpr const char* kKeyRightPattern = "rightPattern";
constexpr const char* kKeyRightReplacement = "rightReplacement";
} // namespace

QVector<CustomRule> loadCustomRules()
{
    QVector<CustomRule> out;
    QSettings settings;
    const QString raw =
        settings.value(AppSettingsKeys::skeletonMirrorCustomRules()).toString();
    if (raw.isEmpty()) return out;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return out;

    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        CustomRule r;
        r.leftPattern = o.value(QLatin1String(kKeyLeftPattern)).toString();
        r.leftReplacement = o.value(QLatin1String(kKeyLeftReplacement)).toString();
        r.rightPattern = o.value(QLatin1String(kKeyRightPattern)).toString();
        r.rightReplacement = o.value(QLatin1String(kKeyRightReplacement)).toString();
        // Drop invalid rules here rather than at match time: a half-written
        // rule would otherwise make every later mirror op quietly skip it,
        // with no indication of which entry is broken.
        if (r.isValid()) out.push_back(r);
    }
    return out;
}

int saveCustomRules(const QVector<CustomRule>& rules)
{
    QJsonArray arr;
    for (const CustomRule& r : rules) {
        if (!r.isValid()) continue;
        QJsonObject o;
        o[QLatin1String(kKeyLeftPattern)] = r.leftPattern;
        o[QLatin1String(kKeyLeftReplacement)] = r.leftReplacement;
        o[QLatin1String(kKeyRightPattern)] = r.rightPattern;
        o[QLatin1String(kKeyRightReplacement)] = r.rightReplacement;
        arr.append(o);
    }
    QSettings settings;
    settings.setValue(AppSettingsKeys::skeletonMirrorCustomRules(),
                      QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    return static_cast<int>(arr.size());
}

} // namespace BoneNaming
