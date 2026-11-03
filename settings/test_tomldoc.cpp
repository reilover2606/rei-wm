#include <cstdio>
#include <cstdlib>

#include "tomldoc.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define CHECK_EQ(a, b) do { QString _a = (a), _b = (b); if (_a != _b) { \
	fprintf(stderr, "%s:%d: '%s' != '%s'\n", __FILE__, __LINE__, qPrintable(_a), qPrintable(_b)); exit(1); } } while (0)

static const char *sample =
	"# rei-wm configuration\n"
	"top = 1\n"
	"\n"
	"[general]\n"
	"backend = \"auto\"   # which backend\n"
	"focus_follows_mouse = false\n"
	"\n"
	"[layout]\n"
	"gap_outer = 10\n"
	"gap_inner = 10\n"
	"\n"
	"[keybindings]\n"
	"\"Mod+Return\" = \"exec foot\"\n"
	"\"Mod+Shift+q\" = \"close\"\n"
	"\n"
	"# [[output]]\n"
	"# name = \"X\"\n"
	"[[output]]\n"
	"name = \"DP-1\"\n"
	"width = 1920\n"
	"height = 1080\n"
	"\n"
	"[[output]]\n"
	"name = \"HDMI-A-1\"\n"
	"scale = 1.5\n";

int main()
{
	TomlDoc d = TomlDoc::parse(sample);
	CHECK_EQ(d.text(), sample);  /* Parsing and printing changes nothing. */

	CHECK_EQ(d.get("general", "backend").toString(), "auto");
	CHECK(d.get("general", "focus_follows_mouse").typeId() == QMetaType::Bool);
	CHECK(d.get("general", "focus_follows_mouse").toBool() == false);
	CHECK(d.get("layout", "gap_outer").toLongLong() == 10);
	CHECK(d.get("", "top").toLongLong() == 1);
	CHECK(!d.has("general", "nothing"));
	CHECK(d.get("keybindings", "Mod+Return").toString() == "exec foot");
	CHECK(d.keys("keybindings") == QStringList({ "Mod+Return", "Mod+Shift+q" }));

	/* Replacing a value keeps the trailing comment and everything else. */
	d.set("general", "backend", "drm");
	CHECK(d.text().contains("backend = \"drm\"   # which backend\n"));
	d.set("layout", "gap_outer", (qlonglong)25);
	CHECK(d.text().contains("gap_outer = 25\n"));
	CHECK(d.text().contains("gap_inner = 10\n"));
	d.set("general", "focus_follows_mouse", true);
	CHECK(d.get("general", "focus_follows_mouse").toBool());

	/* A missing key is added after the section's last key. */
	d.set("layout", "new_windows", "float");
	CHECK(d.text().contains("gap_inner = 10\nnew_windows = \"float\"\n"));
	/* A missing section is created at the end of the file. */
	d.set("toolbar", "enabled", false);
	CHECK(d.get("toolbar", "enabled").toBool() == false);
	CHECK(d.text().contains("[toolbar]\nenabled = false\n"));
	/* A float keeps a decimal point. */
	d.set("input", "pointer_accel", 0.5);
	d.set("input", "zero", 0.0);
	CHECK(d.text().contains("pointer_accel = 0.5\n"));
	CHECK(d.text().contains("zero = 0.0\n"));
	/* Strings are escaped. */
	d.set("toolbar", "font", "Say \"hi\"\\");
	CHECK_EQ(d.get("toolbar", "font").toString(), "Say \"hi\"\\");
	/* Top-level keys stay above the first table. */
	d.set("", "other", (qlonglong)2);
	CHECK(d.text().indexOf("other = 2") < d.text().indexOf("[general]"));

	/* Keybindings: quoted keys, removal. */
	d.set("keybindings", "Mod+x", "close");
	CHECK(d.text().contains("\"Mod+x\" = \"close\"\n"));
	d.remove("keybindings", "Mod+Shift+q");
	CHECK(!d.has("keybindings", "Mod+Shift+q"));
	CHECK(d.has("keybindings", "Mod+Return"));

	/* Arrays of tables. */
	QList<QVariantMap> outs = d.tables("output");
	CHECK(outs.size() == 2);
	CHECK_EQ(outs[0]["name"].toString(), "DP-1");
	CHECK(outs[0]["width"].toLongLong() == 1920);
	CHECK(outs[1]["scale"].toDouble() == 1.5);

	QVariantMap a, b;
	a["name"] = "eDP-1";
	a["mode"] = "1280x720@60";
	a["scale"] = 2.0;
	b["name"] = "DP-2";
	b["enabled"] = false;
	d.setTables("output", { a, b });
	QList<QVariantMap> after = d.tables("output");
	CHECK(after.size() == 2);
	CHECK_EQ(after[0]["mode"].toString(), "1280x720@60");
	CHECK(after[1]["enabled"].toBool() == false);
	CHECK(!d.text().contains("HDMI-A-1"));
	CHECK(d.text().contains("# [[output]]\n# name = \"X\"\n"));  /* A commented example is left alone. */

	/* Nothing else was disturbed by all that. */
	CHECK(d.text().startsWith("# rei-wm configuration\n"));
	CHECK(d.get("layout", "gap_inner").toLongLong() == 10);

	/* Removing every output leaves the rest. */
	d.setTables("output", {});
	CHECK(d.tables("output").isEmpty());
	CHECK(d.has("general", "backend"));

	puts("tomldoc: ok");
	return 0;
}
