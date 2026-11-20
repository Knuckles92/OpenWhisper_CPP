// ui_qt/utils/collapse_animation.py: the shared constants and the run helper.
#include <catch2/catch_test_macros.hpp>

#include <QPropertyAnimation>
#include <QSignalSpy>
#include <QWidget>

#include "ui_widgets/collapse_animation.h"

TEST_CASE("the collapse animation is 300 ms InOutCubic on maximumHeight", "[widget]") {
    QWidget body;
    QPropertyAnimation* anim = fv::createMaxHeightAnimation(&body);
    CHECK(fv::kCollapseDurationMs == 300);
    CHECK(fv::kUnlimitedHeight == 16777215);
    CHECK(fv::kCollapseEasing == QEasingCurve::InOutCubic);
    CHECK(anim->duration() == 300);
    CHECK(anim->easingCurve().type() == QEasingCurve::InOutCubic);
    CHECK(anim->targetObject() == &body);
    CHECK(anim->propertyName() == QByteArray("maximumHeight"));
    CHECK(anim->parent() == &body);
}

TEST_CASE("running the animation starts at the start height and calls back once", "[widget]") {
    QWidget body;
    QObject owner;
    QPropertyAnimation* anim = fv::createMaxHeightAnimation(&body, &owner);
    CHECK(anim->parent() == &owner);
    anim->setDuration(10);

    int calls = 0;
    fv::runMaxHeightAnimation(anim, 120, 0, [&] { ++calls; });
    CHECK(body.maximumHeight() == 120);
    CHECK(anim->state() == QAbstractAnimation::Running);

    QSignalSpy finished(anim, &QAbstractAnimation::finished);
    REQUIRE(finished.wait(2000));
    CHECK(calls == 1);
    CHECK(body.maximumHeight() == 0);

    // A second run replaces the callback of the first.
    int second = 0;
    fv::runMaxHeightAnimation(anim, 0, 50, [&] { ++second; });
    QSignalSpy finishedAgain(anim, &QAbstractAnimation::finished);
    REQUIRE(finishedAgain.wait(2000));
    CHECK(calls == 1);
    CHECK(second == 1);
}
