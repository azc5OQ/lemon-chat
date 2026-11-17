# Writes the launcher icons that show the number of unread messages, for the hybrid Android app (android/) and the
# native Android app (android-native/).
#
# Android cannot draw a number onto the icon of an app that a launcher shows. Both apps therefore declare 101
# launcher entries in their manifest (activity-alias elements): one with the plain icon and one for each count from
# 1 to 99 and "99+". Exactly one entry is enabled at a time; LauncherIconBadge.java in each app enables the entry
# that matches the unread count.
#
# Each badge icon is an adaptive icon (API 26 and newer). Its foreground layer is the plain foreground picture at
# full size with a red badge in the upper right corner. A launcher cuts every adaptive icon to a mask shape (circle,
# rounded square and others); only the middle 72 of the 108 units are visible, and with a round mask only a circle
# of radius 36 around the middle remains. The badge is placed so that it stays complete inside that circle.
#
# The digits are vector outlines taken from desktop/fonts/Inter Medium.ttf (SIL Open Font License) with fontTools.
#
# Usage, from any folder:
#   python generate-launcher-badges.py           writes the files of both apps
#   python generate-launcher-badges.py native    writes the files of one app (hybrid or native)
import sys
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

MOBILE = Path(__file__).resolve().parent.parent
FONT = MOBILE.parent / "desktop" / "fonts" / "Inter Medium.ttf"

APPS = {
    "hybrid": {"folder": MOBILE / "android" / "app" / "src" / "main", "activity": ".MainActivity",
               "background": "@color/ic_launcher_background"},
    "native": {"folder": MOBILE / "android-native" / "app" / "src" / "main", "activity": ".ui.MainActivity",
               "background": "@color/launcher_background"},
}

# counts 1 to 99 have their own icon, 100 stands for "99+"
HIGHEST_COUNT = 100

# badge geometry in the 108 x 108 units of an adaptive icon layer
OUTER_RADIUS = 13.0
RING_WIDTH = 2.0
RIGHT_CENTRE_X = 69.5
CENTRE_Y = 38.5
DIGIT_HEIGHT = 11.5
TEXT_PADDING = 3.5
SHADOW_OFFSET = 1.2

MARK_START = "<!-- badge-aliases-start"
MARK_END = "<!-- badge-aliases-end -->"


def label_for(count):
    """
    @brief text shown in the badge

    @param count number from 1 to HIGHEST_COUNT

    @return the number, or "99+" for HIGHEST_COUNT
    """
    return "99+" if count == HIGHEST_COUNT else str(count)


def text_width(font, text):
    """
    @brief width of the drawn part of a text in icon units, from the left edge of the first glyph outline to the
    right edge of the last one

    @param font the opened font
    @param text characters to measure

    @return (width, left side bearing of the first glyph), both in icon units
    """
    character_map = font.getBestCmap()
    scale = DIGIT_HEIGHT / font["OS/2"].sCapHeight
    names = [character_map[ord(character)] for character in text]

    # sum the advance widths
    advance = sum(font["hmtx"][name][0] for name in names)

    # remove both side bearings
    first_bearing = font["glyf"][names[0]].xMin
    last_right_bearing = font["hmtx"][names[-1]][0] - font["glyf"][names[-1]].xMax
    return (advance - first_bearing - last_right_bearing) * scale, first_bearing * scale


def text_outline(font, text, origin_x, baseline_y):
    """
    @brief vector path of a text in icon units

    @param font the opened font
    @param text characters to draw
    @param origin_x x position of the first glyph's origin
    @param baseline_y y position of the baseline

    @return path data
    """
    glyph_set = font.getGlyphSet()
    character_map = font.getBestCmap()
    scale = DIGIT_HEIGHT / font["OS/2"].sCapHeight
    pen = SVGPathPen(glyph_set)
    advance = origin_x

    # place every glyph
    for character in text:
        name = character_map[ord(character)]

        # flip the y axis
        glyph_set[name].draw(TransformPen(pen, (scale, 0, 0, -scale, advance, baseline_y)))
        advance += glyph_set[name].width * scale
    return pen.getCommands()


def pill_path(left_x, right_x, centre_y, radius):
    """
    @brief closed shape of two half circles joined by straight lines; a circle when left_x equals right_x

    @param left_x centre of the left half circle
    @param right_x centre of the right half circle
    @param centre_y vertical centre
    @param radius radius of both half circles

    @return path data
    """
    top = centre_y - radius
    bottom = centre_y + radius
    return ("M %.2f,%.2f H %.2f A %.2f,%.2f 0 0 1 %.2f,%.2f H %.2f A %.2f,%.2f 0 0 1 %.2f,%.2f Z"
            % (left_x, top, right_x, radius, radius, right_x, bottom, left_x, radius, radius, left_x, top))


def foreground_drawable(font, count):
    """
    @brief layer list with the plain foreground picture and the badge for one count

    @param font the opened font
    @param count number from 1 to HIGHEST_COUNT

    @return XML text
    """
    text = label_for(count)
    width, first_bearing = text_width(font, text)

    # widen the circle into a pill for wide text
    inner_radius = OUTER_RADIUS - RING_WIDTH
    straight_length = max(0.0, width - 2 * (inner_radius - TEXT_PADDING))
    left_x = RIGHT_CENTRE_X - straight_length

    # centre the text in the badge
    origin_x = (left_x + RIGHT_CENTRE_X) / 2 - width / 2 - first_bearing
    digits = text_outline(font, text, origin_x, CENTRE_Y + DIGIT_HEIGHT / 2)

    # write the layers
    return """<?xml version="1.0" encoding="utf-8"?>
<!-- generated by mobile/tools/generate-launcher-badges.py: the plain launcher foreground with the unread badge for "%s" -->
<layer-list xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt">
    <item android:drawable="@mipmap/ic_launcher_foreground" />
    <item>
        <vector android:width="108dp" android:height="108dp"
            android:viewportWidth="108" android:viewportHeight="108">
            <path android:pathData="%s" android:fillColor="#000000" android:fillAlpha="0.28" />
            <path android:pathData="%s" android:fillColor="#ffffff" />
            <path android:pathData="%s">
                <aapt:attr name="android:fillColor">
                    <gradient android:type="linear" android:startX="0" android:startY="%.2f"
                        android:endX="0" android:endY="%.2f"
                        android:startColor="#f2564f" android:endColor="#c62828" />
                </aapt:attr>
            </path>
            <path android:pathData="%s" android:fillColor="#ffffff"
                android:strokeColor="#ffffff" android:strokeWidth="0.45" android:strokeLineJoin="round" />
        </vector>
    </item>
</layer-list>
""" % (text,
       pill_path(left_x, RIGHT_CENTRE_X, CENTRE_Y + SHADOW_OFFSET, OUTER_RADIUS + 0.4),
       pill_path(left_x, RIGHT_CENTRE_X, CENTRE_Y, OUTER_RADIUS),
       pill_path(left_x, RIGHT_CENTRE_X, CENTRE_Y, inner_radius),
       CENTRE_Y - inner_radius, CENTRE_Y + inner_radius,
       digits)


def adaptive_icon(background, count):
    """
    @brief adaptive icon that uses the badge foreground of one count

    @param background colour resource of the background layer
    @param count number from 1 to HIGHEST_COUNT

    @return XML text
    """
    return """<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="%s" />
    <foreground android:drawable="@drawable/ic_launcher_badge_fg_%s" />
</adaptive-icon>
""" % (background, resource_suffix(count))


def resource_suffix(count):
    """
    @brief part of the resource and alias names for one count

    @param count number from 1 to HIGHEST_COUNT

    @return "1" to "99", or "99plus"
    """
    return "99plus" if count == HIGHEST_COUNT else str(count)


def alias_block(activity):
    """
    @brief manifest entries of the badge icons, all disabled

    @param activity name of the activity that every entry opens

    @return XML text between the two marks, including the marks
    """
    lines = [MARK_START + " (generated by mobile/tools/generate-launcher-badges.py, do not edit by hand) -->"]
    for count in range(1, HIGHEST_COUNT + 1):
        suffix = resource_suffix(count)
        lines.append('        <activity-alias android:name=".LauncherBadge%s" android:targetActivity="%s"' % (suffix, activity))
        lines.append('            android:icon="@mipmap/ic_launcher_badge_%s" android:roundIcon="@mipmap/ic_launcher_badge_%s"' % (suffix, suffix))
        lines.append('            android:enabled="false" android:exported="true">')
        lines.append("            <intent-filter>")
        lines.append('                <action android:name="android.intent.action.MAIN" />')
        lines.append('                <category android:name="android.intent.category.LAUNCHER" />')
        lines.append("            </intent-filter>")
        lines.append("        </activity-alias>")
    lines.append("        " + MARK_END)
    return "\n".join(lines)


def write_app(font, name):
    """
    @brief writes the drawables, the adaptive icons and the manifest entries of one app

    @param font the opened font
    @param name key of APPS
    """
    app = APPS[name]
    resources = app["folder"] / "res"
    (resources / "drawable").mkdir(parents=True, exist_ok=True)
    (resources / "mipmap-anydpi-v26").mkdir(parents=True, exist_ok=True)

    # write the icon files
    for count in range(1, HIGHEST_COUNT + 1):
        suffix = resource_suffix(count)
        (resources / "drawable" / ("ic_launcher_badge_fg_%s.xml" % suffix)).write_bytes(foreground_drawable(font, count).encode("utf-8"))
        (resources / "mipmap-anydpi-v26" / ("ic_launcher_badge_%s.xml" % suffix)).write_bytes(adaptive_icon(app["background"], count).encode("utf-8"))

    # replace the manifest entries
    manifest_path = app["folder"] / "AndroidManifest.xml"
    manifest = manifest_path.read_bytes().decode("utf-8")
    start = manifest.index(MARK_START)
    end = manifest.index(MARK_END) + len(MARK_END)
    manifest = manifest[:start] + alias_block(app["activity"]) + manifest[end:]
    manifest_path.write_bytes(manifest.encode("utf-8"))
    print(name + ": " + str(HIGHEST_COUNT) + " badge icons written to " + str(resources))


if __name__ == "__main__":
    font = TTFont(str(FONT))
    names = sys.argv[1:] or list(APPS)
    for app_name in names:
        write_app(font, app_name)
