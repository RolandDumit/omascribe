import QtQuick
import QtQuick.Controls
import QtQuick.Window

// Both document views use the same wheel physics and pixel alignment.
Flickable {
    id: viewport
    property real textScale: 1
    readonly property bool userScrolling: moving || wheelScroll.running || verticalBar.pressed
    signal scrollNavigated()
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {
        id: verticalBar
        policy: ScrollBar.AsNeeded
        // Wheel scrolling moves contentY directly rather than
        // flicking the Flickable, so the bar has to be told about
        // that activity; linger briefly after the last event.
        active: hovered || pressed || wheelScroll.running || scrollLinger.running
    }

    Timer {
        id: scrollLinger
        interval: 600
    }

    // Flickable turns a wheel notch into a flick sized by the small
    // application font, which crawls next to a browser. Reproduce
    // Chromium's wheel physics instead (cc::ScrollOffsetAnimationCurve):
    // each notch moves 3 lines of 40px towards a running target, the
    // animation gets shorter as the outstanding distance grows, and a
    // notch landing mid-animation carries the current velocity into
    // the new curve, so sustained spinning keeps picking up speed.
    readonly property real wheelStep: Math.round(120 * textScale)

    FrameAnimation {
        id: wheelScroll
        running: false

        property real startY: 0
        property real targetY: 0
        property real duration: 0.2
        // Cubic bezier easing; ease-in-out (0.42, 0, 0.58, 1) for a
        // fresh scroll, with y1 tilted on retarget so the curve's
        // initial slope matches the velocity it inherits.
        property real cx1: 0.42
        property real cy1: 0
        readonly property real cx2: 0.58
        readonly property real cy2: 1

        onTriggered: {
            var x = elapsedTime / duration;
            if (x >= 1) {
                viewport.contentY = viewport.snapToPixel(targetY);
                stop();
                return;
            }
            viewport.contentY = viewport.snapToPixel(
                startY + (targetY - startY) * curveY(solveCurve(x)));
        }

        function begin(from, to, dur, slope) {
            startY = from;
            targetY = to;
            duration = dur;
            cx1 = 0.42;
            cy1 = 0.42 * Math.max(-1000, Math.min(1000, slope));
            restart();
        }

        function retarget(newTarget) {
            var s = solveCurve(Math.min(1, elapsedTime / duration));
            var pos = startY + (targetY - startY) * curveY(s);
            var delta = newTarget - pos;
            if (Math.abs(delta) < 0.5) {
                viewport.contentY = newTarget;
                stop();
                return;
            }

            var velocity = curveDY(s) / Math.max(1e-6, curveDX(s))
                * (targetY - startY) / duration;
            var dur = viewport.wheelDuration(delta);
            // When already moving faster than the eased curve would,
            // bound the duration by the time to target at the current
            // velocity; the 2.5x covers the ease-out tail.
            if (velocity !== 0 && delta / velocity > 0)
                dur = Math.min(dur, delta / velocity * 2.5);
            begin(pos, newTarget, dur, velocity * dur / delta);
        }

        // Cubic bezier through (0,0), (cx1,cy1), (cx2,cy2), (1,1),
        // evaluated by Newton-solving the curve parameter from x.
        function curveX(s) { return 3 * s * (1 - s) * ((1 - s) * cx1 + s * cx2) + s * s * s; }
        function curveY(s) { return 3 * s * (1 - s) * ((1 - s) * cy1 + s * cy2) + s * s * s; }
        function curveDX(s) { return 3 * (1 - s) * (1 - s) * cx1 + 6 * (1 - s) * s * (cx2 - cx1) + 3 * s * s * (1 - cx2); }
        function curveDY(s) { return 3 * (1 - s) * (1 - s) * cy1 + 6 * (1 - s) * s * (cy2 - cy1) + 3 * s * s * (1 - cy2); }

        function solveCurve(x) {
            var s = x;
            for (var i = 0; i < 8; ++i) {
                var error = curveX(s) - x;
                if (Math.abs(error) < 0.001)
                    break;
                var d = curveDX(s);
                if (Math.abs(d) < 1e-6)
                    break;
                s = Math.max(0, Math.min(1, s - error / d));
            }
            return s;
        }
    }

    WheelHandler {
        // Wayland compositors route every pointer's scroll through
        // one seat device that Qt classifies as a touchpad, so the
        // device type cannot tell a mouse wheel from two-finger
        // scrolling. Distinguish by event shape instead: discrete
        // wheel notches arrive with only angleDelta set, while
        // finger scrolling carries pixel-precise pixelDelta.
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(wheel) {
            scrollLinger.restart();
            var horizontal = wheel.pixelDelta.x !== 0
                ? wheel.pixelDelta.x : wheel.angleDelta.x / 120 * wheelStep;
            if (horizontal !== 0)
                viewport.contentX = Math.max(0, Math.min(Math.max(0, contentWidth - width), contentX - horizontal));
            if (wheel.pixelDelta.y !== 0) {
                viewport.scrollTo(viewport.clampContentY(viewport.contentY - wheel.pixelDelta.y));
                viewport.scrollNavigated();
            } else
                viewport.scrollByWheel(wheel);
            wheel.accepted = true;
        }
    }

    onMovementStarted: wheelScroll.stop()

    function scrollByWheel(wheel) {
        // High-resolution wheels report fractional notches; feed
        // those through the same animated path, like Chromium does
        // for every wheel-source event.
        var notches = wheel.angleDelta.y / 120;
        if (notches === 0)
            return;

        if (wheelScroll.running) {
            wheelScroll.retarget(clampContentY(wheelScroll.targetY - notches * wheelStep));
            return;
        }

        var target = clampContentY(contentY - notches * wheelStep);
        if (target !== contentY)
            wheelScroll.begin(contentY, target, wheelDuration(target - contentY), 0);
    }

    // Chromium's inverse-delta duration: 200ms for a single notch,
    // ramping down to 100ms once 480px are outstanding.
    function wheelDuration(delta) {
        var pixels = Math.abs(delta) / viewport.textScale;
        return Math.max(6, Math.min(12, 14 - pixels / 60)) / 60;
    }

    function clampContentY(y) {
        return Math.max(0, Math.min(Math.max(0, contentHeight - height), y));
    }

    // Whole device pixels keep natively hinted glyphs from
    // re-rasterizing mid-animation, which reads as shimmer.
    function snapToPixel(y) {
        return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio;
    }

    // Jump to a position, abandoning any wheel animation still running.
    function scrollTo(y) {
        wheelScroll.stop();
        contentY = snapToPixel(y);
    }

}
