/***************************************************************************
//
//    softProjector - an open source media projection software
//    Copyright (C) 2017  Vladislav Kobzar
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation version 3 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
***************************************************************************/

import QtQuick
import QtMultimedia

Rectangle {
    id: dispArea
    visible: true
    color: "#000000"
    anchors.fill: parent

    property int mx: 0
    property int my: 0
    property int mTox: 0
    property int mToy: 0

    property  int rotation1:0
    property  int rotation2:0

    property int tranTime: 500

    // Qt 6 ignores a seek while the file is still loading; keep it until loaded.
    property real pendingPosition: -1

    // Display Conrol signals
    signal exitClicked()
    signal nextClicked()
    signal prevClicked()

    // Video signals
    signal positionChanged(int position)
    signal durationChanged(int duration)
    signal playbackStateChanged(int state)
    signal playbackStopped()

    // Qt 6 keeps a player on the sound device that was the default when it
    // started. HDMI sound goes away with a dropped projector link, so follow
    // the Windows default instead of going silent.
    MediaDevices { id: mediaDevices }

    // Qt 6: the player must be told where to draw (videoOutput) and where to
    // send sound (audioOutput); without these, video stayed black and silent.
    MediaPlayer
    {
        id: player
        objectName: "player"
        videoOutput: vidOut
        audioOutput: AudioOutput { id: audioOut; device: mediaDevices.defaultAudioOutput }
        onPositionChanged:
        {
            dispArea.positionChanged(player.position)
        }
        onDurationChanged:
        {
            dispArea.durationChanged(player.duration)
        }
        onPlaybackStateChanged:
        {
            dispArea.playbackStateChanged(player.playbackState)
        }
        // Qt 6 has no "stopped" signal. Report only a natural end of the file:
        // stop() is also called whenever the screen switches to a song, and
        // must not be reported as the video finishing.
        onMediaStatusChanged:
        {
            if(player.mediaStatus === MediaPlayer.EndOfMedia)
                dispArea.playbackStopped()
            else if(dispArea.pendingPosition >= 0
                    && (player.mediaStatus === MediaPlayer.LoadedMedia
                        || player.mediaStatus === MediaPlayer.BufferedMedia))
            {
                player.setPosition(dispArea.pendingPosition)
                dispArea.pendingPosition = -1
            }
        }
    }

    VideoOutput
    {
        id: vidOut
        objectName: "vidOut"
        anchors.fill: parent
        // Hide when stopped so a frozen last frame never shows under later slides.
        visible: player.playbackState !== MediaPlayer.StoppedState
    }

    Image
    {
        id: backImage1
        objectName: "backImage1"
//        anchors.fill: parent
        cache: false
//        transform: Rotation
//            {
//            id: rotBack1
//            angle: 0
//            axis {x:0; y:0; z:0}
//            origin {x:backImage1/2; y:backImage1/2}
//        }
    }

    Image
    {
        id: backImage2
        objectName: "backImage2"
//        anchors.fill: parent
        cache: false
//        transform: Rotation
//            {
//            id: rotBack2
//            angle: rotation1
//            axis {x:0; y:0; z:0}
//            origin {x:backImage2/2; y:backImage2/2}
//        }
    }

    // ---- Presentation steps (projectordisplayscreen.cpp, renderSlideStep) ----
    // One still replaces another with a simple effect on the part that
    // changed. fx: kind (wipe, split, box, fly, float, zoom), dir, exit,
    // duration, the changed part x y w h and the still's corner sx sy, all in
    // screen pixels; push when both stills move (a slide transition).
    property real fxT: 1
    property var fx: ({ kind: "" })
    property var fxIncoming: null
    property var fxOutgoing: null

    Item
    {
        id: fxLayer
        objectName: "fxLayer"
        anchors.fill: parent
        visible: false

        // Wipe, split and box: the new still, seen through a growing window.
        Item { id: fxClipA; objectName: "fxClipA"; clip: true; Image { id: fxStillA; objectName: "fxStillA"; cache: false } }
        Item { id: fxClipB; objectName: "fxClipB"; clip: true; Image { id: fxStillB; objectName: "fxStillB"; cache: false } }
        // Fly, float and zoom: the changed part by itself, moving.
        Image { id: fxSpriteOut; objectName: "fxSpriteOut"; cache: false }
        Image { id: fxSprite; objectName: "fxSprite"; cache: false }
    }

    NumberAnimation
    {
        id: fxAnim
        target: dispArea
        property: "fxT"
        from: 0
        to: 1
        onFinished: dispArea.fxFinish()
    }

    onFxTChanged: fxUpdate()

    // Videos, sounds and animated pictures of presentation slides, over the still.
    property var slideGifs: []

    Item
    {
        id: slideMediaLayer
        anchors.fill: parent

        Repeater
        {
            model: dispArea.slideGifs
            AnimatedImage
            {
                required property var modelData
                source: modelData.source
                x: modelData.x
                y: modelData.y
                width: modelData.w
                height: modelData.h
                playing: true
            }
        }

        VideoOutput
        {
            id: slideVideo
            objectName: "slideVideo"
            fillMode: VideoOutput.Stretch
            visible: slidePlayer.playbackState !== MediaPlayer.StoppedState && slidePlayer.hasVideo
        }
    }

    MediaPlayer
    {
        id: slidePlayer
        objectName: "slidePlayer"
        videoOutput: slideVideo
        audioOutput: AudioOutput { id: slideVideoSound; device: mediaDevices.defaultAudioOutput }
    }

    MediaPlayer
    {
        id: slideSound
        objectName: "slideSound"
        audioOutput: AudioOutput { id: slideSoundOut; device: mediaDevices.defaultAudioOutput }
    }

    Image
    {
        id: textImage1
        objectName: "textImage1"
//        anchors.fill: parent
//        fillMode: Image.Stretch
//        anchors.centerIn: parent
        cache: false
//        transform: Rotation
//            {
//            id: rotText1
//            angle: 0
//            axis {x:0; y:0; z:0}
//            origin {x:textImage1/2; y:textImage1/2}
//        }
    }

    Image
    {
        id: textImage2
        objectName: "textImage2"
//        anchors.fill: parent
//        fillMode: Image.Stretch
//        anchors.centerIn: parent
        cache: false
//        transform: Rotation
//            {
//            id: rotText2
//            angle: rotation2
//            axis {x:0; y:0; z:0}
//            origin {x:textImage2/2; y:textImage2/2}
//        }
    }

    Rectangle
    {
        id: controls
        objectName: "controls"
//        opacity: 0.5
        color: "#00000000"
        height: 128
        width: height*3 +20

        x:20; y:20

        Image
        {
            id: prevBtn
            objectName: "prevBtn"
            source: "qrc:/icons/icons/controlPrev.png"
            width: parent.height; height: parent.height

            MouseArea
            {
                id: maPrev
                anchors.fill: parent
                hoverEnabled: true
                onHoveredChanged:
                {
                    if(maPrev.containsMouse)
                        prevBtn.source = "qrc:/icons/icons/controlPrevHovered.png"
                    else
                        prevBtn.source= "qrc:/icons/icons/controlPrev.png"
                }
                onPressed:
                {
                    prevBtn.source = "qrc:/icons/icons/controlPrevPressed.png"
                }
                onReleased:
                {
                    prevBtn.source = "qrc:/icons/icons/controlPrevHovered.png"
                }
                onClicked:
                {
                    dispArea.prevClicked()
                }
            }
        }

        Image
        {
            id: nextBtn
            objectName: "nextBtn"
            source: "qrc:/icons/icons/controlNext.png"
            width: prevBtn.width; height: prevBtn.height
            anchors.left: prevBtn.right
            anchors.leftMargin: 10
            anchors.top: prevBtn.top
            opacity: prevBtn.opacity
            MouseArea
            {
                id: maNext
                anchors.fill: parent
                hoverEnabled: true
                onHoveredChanged:
                {
                    if(maNext.containsMouse)
                        nextBtn.source = "qrc:/icons/icons/controlNextHovered.png"
                    else
                        nextBtn.source= "qrc:/icons/icons/controlNext.png"
                }
                onPressed:
                {
                    nextBtn.source = "qrc:/icons/icons/controlNextPressed.png"
                }
                onReleased:
                {
                    nextBtn.source = "qrc:/icons/icons/controlNextHovered.png"
                }
                onClicked:
                {
                    dispArea.nextClicked()
                }
            }
        }

        Image
        {
            id: exitBtn
            objectName: "exitBtn"
            source: "qrc:/icons/icons/controlExit.png"
            width: prevBtn.width; height: prevBtn.height
            anchors.left: nextBtn.right
            anchors.leftMargin: 10
            anchors.top: prevBtn.top
            opacity: prevBtn.opacity
            MouseArea
            {
                id: maExit
                anchors.fill: parent
                hoverEnabled: true
                onHoveredChanged:
                {
                    if(maExit.containsMouse)
                        exitBtn.source = "qrc:/icons/icons/controlExitHovered.png"
                    else
                        exitBtn.source= "qrc:/icons/icons/controlExit.png"
                }
                onPressed:
                {
                    exitBtn.source = "qrc:/icons/icons/controlExitPressed.png"
                }
                onReleased:
                {
                    exitBtn.source = "qrc:/icons/icons/controlExitHovered.png"
                }
                onClicked:
                {
                    dispArea.exitClicked()
                }
            }
        }
    }

    // The time of day in the top right corner: on the back screen only
    // (General settings, "Show the time"), for the people preaching. The
    // stream and the back-screen mirror don't show it.
    property bool clockVisible: false

    Rectangle
    {
        id: clockBox
        objectName: "clockBox"
        visible: dispArea.clockVisible
        z: 10
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: dispArea.height * 0.025
        width: clockText.implicitWidth + dispArea.height * 0.05
        height: clockText.implicitHeight + dispArea.height * 0.012
        radius: height * 0.18
        color: "#b3000000"

        Text
        {
            id: clockText
            objectName: "clockText"
            anchors.centerIn: parent
            color: "#ffffff"
            font.pixelSize: Math.max(16, dispArea.height * 0.065)
            font.bold: true
        }

        Timer
        {
            interval: 1000
            repeat: true
            running: clockBox.visible
            triggeredOnStart: true
            onTriggered: clockText.text = new Date().toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
        }
    }

    SequentialAnimation
    {
        id:seqFade1to2
        running: false
        NumberAnimation { target: textImage1; property: "opacity"; to: 0.0; duration: tranTime/2;}
        NumberAnimation { target: textImage2; property: "opacity"; to: 1.0; duration: tranTime/2;}
    }

    SequentialAnimation
    {
        id:seqFade2to1
        running: false
        NumberAnimation { target: textImage2; property: "opacity"; to: 0.0; duration: tranTime/2;}
        NumberAnimation { target: textImage1; property: "opacity"; to: 1.0; duration: tranTime/2;}
    }

    ParallelAnimation
    {
        id:parFade1to2
        running: false
        NumberAnimation { target: textImage1; property: "opacity"; to: 0.0; duration: tranTime;}
        NumberAnimation { target: textImage2; property: "opacity"; to: 1.0; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:parFade2to1
        running: false
        NumberAnimation { target: textImage1; property: "opacity"; to: 1.0; duration: tranTime;}
        NumberAnimation { target: textImage2; property: "opacity"; to: 0.0; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:parBackFade1to2
        running: false
        NumberAnimation { target: backImage1; property: "opacity"; to: 0.0; duration: tranTime;}
        NumberAnimation { target: backImage2; property: "opacity"; to: 1.0; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:parBackFade2to1
        running: false
        NumberAnimation { target: backImage1; property: "opacity"; to: 1.0; duration: tranTime;}
        NumberAnimation { target: backImage2; property: "opacity"; to: 0.0; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:moveTextX1to2
        running:false
        NumberAnimation { target: textImage1; property: "x"; to: mTox; duration: tranTime;}
        NumberAnimation { target: textImage2; property: "x"; to: mx; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:moveTextX2to1
        running:false
        NumberAnimation { target: textImage1; property: "x"; to: mx; duration: tranTime;}
        NumberAnimation { target: textImage2; property: "x"; to: mTox; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:moveTextY1to2
        running:false
        NumberAnimation { target: textImage1; property: "y"; to: mToy; duration: tranTime;}
        NumberAnimation { target: textImage2; property: "y"; to: my; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:moveTextY2to1
        running:false
        NumberAnimation { target: textImage1; property: "y"; to: my; duration: tranTime;}
        NumberAnimation { target: textImage2; property: "y"; to: mToy; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:moveBackX1to2
        running:false
        NumberAnimation { target: backImage1; property: "x"; to: mTox; duration: tranTime;}
        NumberAnimation { target: backImage2; property: "x"; to: mx; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:moveBackX2to1
        running:false
        NumberAnimation { target: backImage1; property: "x"; to: mx; duration: tranTime;}
        NumberAnimation { target: backImage2; property: "x"; to: mTox; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:moveBackY1to2
        running:false
        NumberAnimation { target: backImage1; property: "y"; to: mToy; duration: tranTime;}
        NumberAnimation { target: backImage2; property: "y"; to: my; duration: tranTime;}
    }

    ParallelAnimation
    {
        id:moveBackY2to1
        running:false
        NumberAnimation { target: backImage1; property: "y"; to: my; duration: tranTime;}
        NumberAnimation { target: backImage2; property: "y"; to: mToy; duration: tranTime;}
    }

    SequentialAnimation
    {
        id:rotate1to2
        running: false
//        NumberAnimation{ target: rotText1; property: "angle"; from:0; to:90; duration: 250}
//        NumberAnimation{ target: rotText2; property: "angle"; from:270; to:360; duration: 250}

        NumberAnimation{ target: dispArea; properties: "rotation1"; from:0; to:90; duration: 1000}
        NumberAnimation{ target: dispArea; properties: "rotation2"; from:270; to:360; duration: 1000}
    }

    SequentialAnimation
    {
        id:rotate2to1
        running: false
//        NumberAnimation{ target: rotText2; property: "angle"; from:0; to:90; duration: 250}
//        NumberAnimation{ target: rotText1; property: "angle"; from:270; to:360; duration: 250}
        NumberAnimation{ target: dispArea; properties: "rotation2"; from:0; to:90; duration: 1000}
        NumberAnimation{ target: dispArea; properties: "rotation1"; from:270; to:360; duration: 1000}
    }

    function fxWindow(clip, still, cx, cy, cw, ch)
    {
        clip.x = cx
        clip.y = cy
        clip.width = cw
        clip.height = ch
        still.x = fx.sx - cx
        still.y = fx.sy - cy
    }

    function fxUpdate()
    {
        var f = fx, t = fxT
        if(f.kind === "")
            return
        var x = f.x, y = f.y, w = f.w, h = f.h
        if(f.kind === "wipe")
        {
            if(f.dir === "left") fxWindow(fxClipA, fxStillA, x, y, w * t, h)
            else if(f.dir === "right") fxWindow(fxClipA, fxStillA, x + w * (1 - t), y, w * t, h)
            else if(f.dir === "top") fxWindow(fxClipA, fxStillA, x, y, w, h * t)
            else fxWindow(fxClipA, fxStillA, x, y + h * (1 - t), w, h * t)
        }
        else if(f.kind === "box")
            fxWindow(fxClipA, fxStillA, x + w * (1 - t) / 2, y + h * (1 - t) / 2, w * t, h * t)
        else if(f.kind === "split")
        {
            if(f.dir === "vertical")
            {
                fxWindow(fxClipA, fxStillA, x + w / 2 * (1 - t), y, w / 2 * t, h)
                fxWindow(fxClipB, fxStillB, x + w / 2, y, w / 2 * t, h)
            }
            else
            {
                fxWindow(fxClipA, fxStillA, x, y + h / 2 * (1 - t), w, h / 2 * t)
                fxWindow(fxClipB, fxStillB, x, y + h / 2, w, h / 2 * t)
            }
        }
        else
        {
            // away: 1 = as far from its place as it gets, 0 = in place
            var away = f.exit ? t : 1 - t
            var dx = 0, dy = 0
            if(f.kind === "fly")
            {
                if(f.dir === "left") dx = f.push ? -dispArea.width : -(x + w)
                else if(f.dir === "right") dx = f.push ? dispArea.width : dispArea.width - x
                else if(f.dir === "top") dy = f.push ? -dispArea.height : -(y + h)
                else dy = f.push ? dispArea.height : dispArea.height - y
            }
            else if(f.kind === "float")
                dy = (f.dir === "down" ? -1 : 1) * dispArea.height * 0.08
            fxSprite.x = x + dx * away
            fxSprite.y = y + dy * away
            fxSprite.scale = f.kind === "zoom" ? 1 - 0.7 * away : 1
            fxSprite.opacity = f.kind === "fly" ? 1 : 1 - away
            if(f.push)
            {
                fxSpriteOut.x = x - dx * (1 - away)
                fxSpriteOut.y = y - dy * (1 - away)
            }
        }
    }

    function transitionBackStep(to2, spec)
    {
        fxFinish()
        fx = spec
        fxIncoming = to2 ? backImage2 : backImage1
        fxOutgoing = to2 ? backImage1 : backImage2
        fxIncoming.x = spec.sx
        fxIncoming.y = spec.sy
        var moving = spec.kind === "fly" || spec.kind === "float" || spec.kind === "zoom"
        fxSprite.visible = moving
        fxSpriteOut.visible = moving && spec.push
        fxClipA.visible = !moving
        fxClipB.visible = spec.kind === "split"
        // Leaving: the still without it is underneath at once. Arriving: the
        // old still stays until the effect is over. Both moving: neither.
        fxIncoming.opacity = moving && spec.exit && !spec.push ? 1 : 0
        fxOutgoing.opacity = spec.push || (moving && spec.exit) ? 0 : 1
        fxLayer.visible = true
        fxT = 0
        fxUpdate()
        fxAnim.duration = spec.duration
        fxAnim.easing.type = moving ? (spec.exit ? Easing.InCubic : Easing.OutCubic) : Easing.InOutSine
        fxAnim.restart()
    }

    // Ends the effect where it would have ended: the new still alone.
    function fxFinish()
    {
        if(!fxLayer.visible)
            return
        fxAnim.stop()
        fxLayer.visible = false
        fx = { kind: "" }
        if(fxIncoming && fxOutgoing)
        {
            fxIncoming.opacity = 1
            fxOutgoing.opacity = 0
        }
    }

    // list: kind (video, audio, gif), source, x y w h in screen pixels, volume, loop, muted
    function playSlideMedia(list)
    {
        var gifs = [], video = null, sound = null
        for(var i = 0; i < list.length; ++i)
        {
            var m = list[i]
            if(m.kind === "gif") gifs.push(m)
            else if(m.kind === "video" && !video) video = m
            else if(m.kind === "audio" && !sound) sound = m
        }
        // Pictures that stay keep moving instead of starting again.
        if(JSON.stringify(gifs) !== JSON.stringify(slideGifs))
            slideGifs = gifs
        slidePlayer.stop()
        slideSound.stop()
        if(!video)
            slidePlayer.source = ""
        if(!sound)
            slideSound.source = ""
        if(video)
        {
            slideVideo.x = video.x
            slideVideo.y = video.y
            slideVideo.width = video.w
            slideVideo.height = video.h
            slideVideoSound.volume = video.volume
            slideVideoSound.muted = video.muted
            slidePlayer.loops = video.loop ? MediaPlayer.Infinite : 1
            slidePlayer.source = video.source
            slidePlayer.play()
        }
        if(sound)
        {
            slideSoundOut.volume = sound.volume
            slideSoundOut.muted = sound.muted
            slideSound.loops = sound.loop ? MediaPlayer.Infinite : 1
            slideSound.source = sound.source
            slideSound.play()
        }
    }

    // Stops them and lets go of the files, so a deleted slide show's video
    // can be deleted with it.
    function stopSlideMedia()
    {
        slidePlayer.stop()
        slideSound.stop()
        slidePlayer.source = ""
        slideSound.source = ""
        if(slideGifs.length > 0)
            slideGifs = []
    }

    function stopTransitions()
    {
        fxFinish()
        seqFade1to2.stop()
        seqFade2to1.stop()
        parFade1to2.stop()
        parFade2to1.stop()
        parBackFade1to2.stop()
        parBackFade2to1.stop()
    }

    function transitionText1to2(tranType)
    {
        if(tranType === 1)
        {
            textImage2.opacity = 0.0
            parFade1to2.start()
        }
        else if(tranType === 2)
        {
            textImage2.opacity = 0.0
            seqFade1to2.start()
        }
        else if(tranType === 3)
        {
            mTox = mx + parent.width
            textImage1.y = my
            textImage1.x = mx
            textImage2.y = my
            textImage2.x = mx - parent.width
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            moveTextX1to2.start()
        }
        else if(tranType === 4)
        {
            mTox = mx - parent.width
            textImage1.y = my
            textImage1.x = mx
            textImage2.y = my
            textImage2.x = mx + parent.width
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            moveTextX1to2.start()
        }
        else if(tranType === 5)
        {
            mToy = my - parent.height
            textImage1.y = my
            textImage1.x = mx
            textImage2.y = my + parent.height
            textImage2.x = mx
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            moveTextY1to2.start()
        }
        else if(tranType === 6)
        {
            mToy = my + parent.height
            textImage1.y = my
            textImage1.x = mx
            textImage2.y = my - parent.height
            textImage2.x = mx
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            moveTextY1to2.start()
        }
        else if(tranType === "rotate")
        {
            rotText1.axis.x = 0
            rotText1.axis.y = 1
            rotText2.axis.x = 0
            rotText2.axis.y = 1
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            rotate1to2.start()
        }
        else
        {
            textImage1.opacity = 0.0
            textImage2.opacity = 1.0
            textImage1.x = parent.x
            textImage1.y = parent.y
            textImage2.x = parent.x
            textImage2.y = parent.y
        }
    }

    function transitionText2to1(tranType)
    {
        if(tranType === 1)
        {
            textImage1.opacity = 0.0
            parFade2to1.start()
        }
        else if(tranType === 2)
        {
            textImage1.opacity = 0.0
            seqFade2to1.start()
        }
        else if(tranType === 3)
        {
            mTox = mx + parent.width
            textImage1.y = my
            textImage1.x = mx - parent.width
            textImage2.y = my
            textImage2.x = mx
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            moveTextX2to1.start()
        }
        else if(tranType === 4)
        {
            mTox = mx - parent.width
            textImage1.y = my
            textImage1.x = mx + parent.width
            textImage2.y = my
            textImage2.x = mx
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            moveTextX2to1.start()
        }
        else if(tranType === 5)
        {
            mToy = my - parent.height
            textImage1.y = my + parent.height
            textImage1.x = mx
            textImage2.y = my
            textImage2.x = mx
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            moveTextY2to1.start()
        }
        else if(tranType === 6)
        {
            mToy = my + parent.height
            textImage1.y = my - parent.height
            textImage1.x = mx
            textImage2.y = my
            textImage2.x = mx
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            moveTextY2to1.start()
        }
        else if(tranType === "rotate")
        {
            rotText1.axis.x = 0
            rotText1.axis.y = 1
            rotText2.axis.x = 0
            rotText2.axis.y = 1
            textImage1.opacity = 1.0
            textImage2.opacity = 1.0
            rotate2to1.start()
        }
        else
        {
            textImage1.opacity = 1.0
            textImage2.opacity = 0.0
            textImage1.x = parent.x
            textImage1.y = parent.y
            textImage2.x = parent.x
            textImage2.y = parent.y
        }
    }

    function transitionBack1to2(tranType)
    {
        if(tranType === 1 || tranType === 2)
        {
            backImage2.opacity = 0.0
            parBackFade1to2.start()
        }
        else if(tranType === 3)
        {
            mTox = mx + parent.width
            backImage1.y = my
            backImage1.x = mx
            backImage2.y = my
            backImage2.x = mx - parent.width
            backImage1.opacity = 1.0
            backImage2.opacity = 1.0
            moveBackX1to2.start()
        }
        else if(tranType === 4)
        {
            mTox = mx - parent.width
            backImage1.y = my
            backImage1.x = mx
            backImage2.y = my
            backImage2.x = mx + parent.width
            backImage1.opacity = 1.0
            backImage2.opacity = 1.0
            moveBackX1to2.start()
        }
        else if(tranType === 5)
        {
            mToy = my - parent.height
            backImage1.y = my
            backImage1.x = mx
            backImage2.y = my + parent.height
            backImage2.x = mx
            backImage1.opacity = 1.0
            backImage2.opacity = 1.0
            moveBackY1to2.start()
        }
        else if(tranType === 6)
        {
            mToy = my + parent.height
            backImage1.y = my
            backImage1.x = mx
            backImage2.y = my - parent.height
            backImage2.x = mx
            backImage1.opacity = 1.0
            backImage2.opacity = 1.0
            moveBackY1to2.start()
        }
        else
        {
            backImage1.opacity = 0.0
            backImage2.opacity = 1.0
            backImage1.x = parent.x
            backImage1.y = parent.y
            backImage2.x = parent.x
            backImage2.y = parent.y
        }
    }

    function transitionBack2to1(tranType)
    {
        if(tranType === 1 || tranType === 2)
        {
            backImage1.opacity = 0.0
            parBackFade2to1.start()
        }
        else if(tranType === 3)
        {
            mTox = mx + parent.width
            backImage1.y = my
            backImage1.x = mx - parent.width
            backImage2.y = my
            backImage2.x = mx
            backImage1.opacity = 1.0
            backImage2.opacity = 1.0
            moveBackX2to1.start()
        }
        else if(tranType === 4)
        {
            mTox = mx - parent.width
            backImage1.y = my
            backImage1.x = mx + parent.width
            backImage2.y = my
            backImage2.x = mx
            backImage1.opacity = 1.0
            backImage2.opacity = 1.0
            moveBackX2to1.start()
        }
        else if(tranType === 5)
        {
            mToy = my - parent.height
            backImage1.y = my + parent.height
            backImage1.x = mx
            backImage2.y = my
            backImage2.x = mx
            backImage1.opacity = 1.0
            backImage2.opacity = 1.0
            moveBackY2to1.start()
        }
        else if(tranType === 6)
        {
            mToy = my + parent.height
            backImage1.y = my - parent.height
            backImage1.x = mx
            backImage2.y = my
            backImage2.x = mx
            backImage1.opacity = 1.0
            backImage2.opacity = 1.0
            moveBackY2to1.start()
        }
        else
        {
            backImage1.opacity = 1.0
            backImage2.opacity = 0.0
            backImage1.x = parent.x
            backImage1.y = parent.y
            backImage2.x = parent.x
            backImage2.y = parent.y
        }
    }

    // loopCount: MediaPlayer.Infinite (-1) or a play count (1 = once).
    // fill: VideoOutput fill mode (0 = stretch, 1 = keep aspect, 2 = crop).
    function loadVideo(source, loopCount, volume, fill)
    {
        player.stop()
        pendingPosition = -1
        player.source = source
        player.loops = loopCount
        audioOut.volume = volume
        vidOut.fillMode = fill
    }

    function playVideo()
    {
        if(player.playbackState === MediaPlayer.StoppedState
                || player.playbackState === MediaPlayer.PausedState)
        {
            player.play()
        }

    }

    function stopVideo()
    {
        if(player.playbackState === MediaPlayer.PlayingState
                || player.playbackState === MediaPlayer.PausedState)
        {
            player.stop()
        }
    }

    function setVideoVolume(level)
    {
        audioOut.volume = level
    }

    function setVideoMuted(toMute)
    {
        audioOut.muted = toMute
    }

    function setVideoPosition(position)
    {
        if(player.mediaStatus === MediaPlayer.LoadingMedia)
            pendingPosition = position
        else
            player.setPosition(position)
    }

    function pauseVideo()
    {
        if(player.playbackState === MediaPlayer.PlayingState)
        {
            player.pause()
        }
    }

    function positionControls(iX,iY,iSize,dOpacity)
    {
        controls.height = iSize;
        controls.opacity = dOpacity;
        controls.x = iX;
        controls.y = iY;
    }

    function setControlsVisible(isVisible)
    {
        //controls.opacity = dOpacity;
        controls.visible = isVisible;
    }
}

