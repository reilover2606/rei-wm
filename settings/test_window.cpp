/*
 * The settings window, driven offscreen: load a config, change fields, save, and check
 * that the file keeps its comments, only the changed values moved, and that rei-wm
 * itself accepts the result. Also that an invalid value is refused and nothing is written.
 */
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "mainwindow.h"
#include "themeeditor.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static QString slurp(const QString &p)
{
	QFile f(p);
	CHECK(f.open(QIODevice::ReadOnly));
	return QString::fromUtf8(f.readAll());
}

template <class T> static T *field(MainWindow &w, const char *name)
{
	T *t = w.findChild<T *>(name);
	if (!t) {
		fprintf(stderr, "no widget named %s\n", name);
		exit(1);
	}
	return t;
}

int main(int argc, char **argv)
{
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	CHECK(argc == 2);
	QString wm = argv[1];

	QTemporaryDir dir;
	CHECK(dir.isValid());
	QString path = dir.path() + "/config.toml";
	{
		QFile f(path);
		CHECK(f.open(QIODevice::WriteOnly));
		f.write("# my config, hand edited\n"
			"[general]\n"
			"backend = \"auto\"   # keep auto\n"
			"[layout]\n"
			"gap_outer = 10\n"
			"gap_inner = 10\n"
			"[keybindings]\n"
			"\"Mod+Return\" = \"exec foot\"\n"
			"[[output]]\n"
			"name = \"DP-1\"\n"
			"width = 1920\n"
			"height = 1080\n");
	}

	MainWindow w(path, wm);
	/* Values come from the file, or the defaults when the file does not say. */
	CHECK(field<QSpinBox>(w, "layout.gap_outer")->value() == 10);
	CHECK(field<QComboBox>(w, "general.backend")->currentText() == "auto");
	CHECK(field<QSpinBox>(w, "toolbar.thickness")->value() == 28);          /* default */
	CHECK(field<QCheckBox>(w, "input.tap_to_click")->isChecked());          /* default */
	CHECK(field<QTableWidget>(w, "keys")->rowCount() == 1);
	CHECK(field<QTableWidget>(w, "outputs")->rowCount() == 1);
	CHECK(field<QTableWidget>(w, "outputs")->item(0, 1)->text() == "1920x1080");
	CHECK(!w.dirty());

	/* Saving without changing anything leaves the file byte for byte as it was. */
	QString before = slurp(path);
	QString err;
	CHECK(w.save(&err));
	CHECK(slurp(path) == before);

	/* Change some things. */
	field<QSpinBox>(w, "layout.gap_outer")->setValue(24);
	field<QCheckBox>(w, "general.focus_follows_mouse")->setChecked(false);   /* (on by default) */
	field<QComboBox>(w, "toolbar.position")->setCurrentText("bottom");
	field<QWidget>(w, "input.keyboard_layout")->findChild<QLineEdit *>("edit")->setText("us,de");
	field<QDoubleSpinBox>(w, "input.pointer_accel")->setValue(0.5);
	auto *keys = field<QTableWidget>(w, "keys");
	keys->insertRow(1);
	keys->setItem(1, 0, new QTableWidgetItem("Mod+x"));
	keys->setItem(1, 1, new QTableWidgetItem("close"));
	CHECK(w.dirty());
	CHECK(w.save(&err));
	CHECK(!w.dirty());

	QString after = slurp(path);
	CHECK(after.startsWith("# my config, hand edited\n"));
	CHECK(after.contains("backend = \"auto\"   # keep auto\n"));  /* untouched line, comment kept */
	CHECK(after.contains("gap_outer = 24\n"));
	CHECK(after.contains("gap_inner = 10\n"));
	CHECK(after.contains("focus_follows_mouse = false\n"));
	CHECK(after.contains("[toolbar]\nposition = \"bottom\"\n"));
	CHECK(after.contains("keyboard_layout = \"us,de\"\n"));
	CHECK(after.contains("pointer_accel = 0.5\n"));
	CHECK(after.contains("\"Mod+x\" = \"close\"\n"));
	CHECK(after.contains("\"Mod+Return\" = \"exec foot\"\n"));
	CHECK(after.contains("width = 1920\n"));  /* the output block was not touched */

	/* A second window on the same file sees the saved values. */
	MainWindow w2(path, wm);
	CHECK(field<QSpinBox>(w2, "layout.gap_outer")->value() == 24);
	CHECK(field<QComboBox>(w2, "toolbar.position")->currentText() == "bottom");
	CHECK(field<QTableWidget>(w2, "keys")->rowCount() == 2);

	/* An invalid key binding is caught by rei-wm's own check and nothing is written. */
	QString good = slurp(path);
	auto *keys2 = field<QTableWidget>(w2, "keys");
	keys2->insertRow(keys2->rowCount());
	keys2->setItem(keys2->rowCount() - 1, 0, new QTableWidgetItem("Mod+NoSuchKey"));
	keys2->setItem(keys2->rowCount() - 1, 1, new QTableWidgetItem("close"));
	QString why;
	CHECK(!w2.save(&why));
	CHECK(!why.isEmpty());
	CHECK(slurp(path) == good);
	fprintf(stderr, "rejected as expected: %s\n", qPrintable(why));

	/* Monitors: edit and save the block. */
	MainWindow w3(path, wm);
	auto *outs = field<QTableWidget>(w3, "outputs");
	outs->item(0, 1)->setText("2560x1440@144");
	outs->item(0, 2)->setText("1.5");
	CHECK(w3.save(&err));
	QString outText = slurp(path);
	CHECK(outText.contains("mode = \"2560x1440@144\"\n"));
	CHECK(outText.contains("scale = 1.5\n"));
	CHECK(!outText.contains("width = 1920"));

	/* Start menu shortcuts: add, edit, reorder and delete. */
	{
		QString spath = dir.path() + "/shortcuts.toml";
		QFile f(spath);
		CHECK(f.open(QIODevice::WriteOnly));
		f.write("[start_menu]\nlabel = \"Go\"\n"
			"[[shortcut]]\nname = \"Terminal\"\ncommand = \"foot\"\n"
			"[[shortcut]]\nname = \"Browser\"\ncommand = \"firefox-bin\"\n");
		f.close();
		MainWindow ws(spath, wm);
		auto *sc = field<QTableWidget>(ws, "shortcuts");
		CHECK(sc->rowCount() == 2);
		CHECK(sc->item(0, 0)->text() == "Terminal" && sc->item(1, 1)->text() == "firefox-bin");
		CHECK(field<QWidget>(ws, "start_menu.label")->findChild<QLineEdit *>("edit")->text() == "Go");
		CHECK(!ws.dirty());
		QString same = slurp(spath);
		CHECK(ws.save(&err));
		CHECK(slurp(spath) == same);   /* untouched: byte for byte */

		if (qEnvironmentVariableIsSet("REI_SHOT_DIR")) {   /* for looking at, not for checking */
			auto *tabs = ws.findChild<QTabWidget *>();
			CHECK(tabs);
			for (int i = 0; i < tabs->count(); i++)
				if (tabs->tabText(i) == "Start menu")
					tabs->setCurrentIndex(i);
			ws.resize(820, 600);
			ws.show();
			CHECK(ws.grab().save(qEnvironmentVariable("REI_SHOT_DIR") + "/settings-start-menu.png"));
		}

		/* Add one (with quotes and a backslash in its command), edit another, move it, then save. */
		field<QPushButton>(ws, "shortcut_add")->click();
		CHECK(sc->rowCount() == 3);
		sc->item(2, 0)->setText("Say hi");
		sc->item(2, 1)->setText("sh -c 'echo \"hi\\n\"'");
		sc->item(1, 1)->setText("firefox-bin --private-window");
		CHECK(ws.dirty());
		sc->setCurrentCell(2, 0);
		field<QPushButton>(ws, "shortcut_up")->click();   /* "Say hi" now sits between the other two */
		CHECK(sc->item(1, 0)->text() == "Say hi");
		CHECK(ws.save(&err));
		QString st = slurp(spath);
		CHECK(st.contains("label = \"Go\"\n"));
		CHECK(st.indexOf("name = \"Terminal\"") < st.indexOf("name = \"Say hi\""));
		CHECK(st.indexOf("name = \"Say hi\"") < st.indexOf("name = \"Browser\""));
		CHECK(st.contains("command = \"firefox-bin --private-window\""));

		/* Another window reads exactly what was typed back, escapes and all. */
		MainWindow ws2(spath, wm);
		auto *sc2 = field<QTableWidget>(ws2, "shortcuts");
		CHECK(sc2->rowCount() == 3);
		CHECK(sc2->item(1, 1)->text() == "sh -c 'echo \"hi\\n\"'");

		/* Delete one and a blank row is ignored. */
		sc2->setCurrentCell(0, 0);
		sc2->selectRow(0);
		field<QPushButton>(ws2, "shortcut_remove")->click();
		field<QPushButton>(ws2, "shortcut_add")->click();   /* left blank */
		CHECK(ws2.save(&err));
		QString st2 = slurp(spath);
		CHECK(!st2.contains("Terminal"));
		CHECK(st2.contains("Say hi") && st2.contains("Browser"));
		MainWindow ws3(spath, wm);
		CHECK(field<QTableWidget>(ws3, "shortcuts")->rowCount() == 2);

		/* A half-filled row is refused by rei-wm's own check, and the file is left alone. */
		QString keep = slurp(spath);
		field<QPushButton>(ws3, "shortcut_add")->click();
		field<QTableWidget>(ws3, "shortcuts")->item(2, 0)->setText("No command");
		QString why2;
		CHECK(!ws3.save(&why2));
		CHECK(!why2.isEmpty());
		CHECK(slurp(spath) == keep);

		/* Removing every shortcut removes the tables. */
		MainWindow ws4(spath, wm);
		auto *sc4 = field<QTableWidget>(ws4, "shortcuts");
		sc4->selectAll();
		field<QPushButton>(ws4, "shortcut_remove")->click();
		CHECK(ws4.save(&err));
		CHECK(!slurp(spath).contains("[[shortcut]]"));
	}

	/* The new tabs' fields exist and carry rei-wm's defaults. */
	{
		MainWindow wn(path, wm);
		CHECK(field<QCheckBox>(wn, "messages.enabled")->isChecked());
		CHECK(field<QComboBox>(wn, "messages.position")->currentText() == "right");
		CHECK(field<QDoubleSpinBox>(wn, "messages.duration")->value() == 10.0);
		CHECK(field<QSpinBox>(wn, "messages.max_visible")->value() == 6);
		CHECK(field<QCheckBox>(wn, "toolbar.sysinfo")->isChecked());
		CHECK(field<QCheckBox>(wn, "audio.autostart")->isChecked());
		CHECK(field<QSpinBox>(wn, "audio.max_volume")->value() == 100);
		field<QComboBox>(wn, "messages.position")->setCurrentText("left");
		field<QDoubleSpinBox>(wn, "messages.duration")->setValue(4.5);
		field<QCheckBox>(wn, "toolbar.sysinfo")->setChecked(false);
		field<QCheckBox>(wn, "audio.autostart")->setChecked(false);
		CHECK(wn.save(&err));
		QString t = slurp(path);
		CHECK(t.contains("[messages]\nposition = \"left\"\nduration = 4.5\n"));
		CHECK(t.contains("sysinfo = false\n") && t.contains("autostart = false\n"));
	}

	/* The theme editor: effective values from rei-wm, edits written as [theme.<element>] keys, the rest left alone. */
	{
		MainWindow wt(path, wm);
		auto *radius = field<QDoubleSpinBox>(wt, "theme.tray_tile.radius");
		CHECK(radius->value() == 4);   /* rei-wm's built-in look */
		CHECK(field<QWidget>(wt, "theme.toolbar.color") != nullptr);
		CHECK(field<QCheckBox>(wt, "theme.menu_row.icon")->isChecked());          /* iconable elements have an icon switch */
		CHECK(wt.findChild<QDoubleSpinBox *>("theme.tooltip.icon_size") == nullptr);   /* others do not */
		CHECK(field<QDoubleSpinBox>(wt, "theme.menu_row.glow_size")->value() > 0);
		CHECK(!wt.dirty());
		QString before2 = slurp(path);
		CHECK(wt.save(&err));
		CHECK(slurp(path) == before2);   /* nothing changed, nothing written */

		if (qEnvironmentVariableIsSet("REI_SHOT_DIR")) {   /* for looking at, not for checking */
			auto *tabs = wt.findChild<QTabWidget *>();
			for (int i = 0; i < tabs->count(); i++)
				if (tabs->tabText(i) == "Theme")
					tabs->setCurrentIndex(i);
			wt.findChild<ThemeEditor *>("theme_editor")->findChild<QListWidget *>("theme_elements")->setCurrentRow(6);   /* Menu rows */
			wt.resize(1040, 900);
			wt.show();
			QString e;
			CHECK(wt.findChild<ThemeEditor *>("theme_editor")->refreshPreviewNow(&e));
			CHECK(wt.grab().save(qEnvironmentVariable("REI_SHOT_DIR") + "/settings-theme.png"));
		}

		radius->setValue(9);
		field<QComboBox>(wt, "theme.dialog.shape")->setCurrentText("bevel");
		field<QDoubleSpinBox>(wt, "theme.menu_row.glow_size")->setValue(0);
		field<QWidget>(wt, "theme.messages.shadow_color");
		CHECK(wt.dirty());
		auto *te = wt.findChild<ThemeEditor *>("theme_editor");
		CHECK(te && te->changed());
		QString perr;
		CHECK(te->refreshPreviewNow(&perr));   /* rei-wm draws the preview with the edits */
		CHECK(te->previewSize().width() >= 900 && te->previewSize().height() >= 400);
		CHECK(wt.save(&err));
		QString tt = slurp(path);
		CHECK(tt.contains("[theme.tray_tile]\nradius = 9.0\n"));
		CHECK(tt.contains("[theme.dialog]\nshape = \"bevel\"\n"));
		CHECK(tt.contains("[theme.menu_row]\nglow_size = 0.0\n"));
		CHECK(!tt.contains("[theme.toolbar]"));   /* untouched elements are not written */
		MainWindow wt2(path, wm);
		CHECK(field<QDoubleSpinBox>(wt2, "theme.tray_tile.radius")->value() == 9);
		CHECK(field<QComboBox>(wt2, "theme.dialog.shape")->currentText() == "bevel");
		/* The flavor icons folder is a plain [theme] setting. */
		field<QWidget>(wt2, "theme.flavor_icons")->findChild<QLineEdit *>("edit")->setText("/tmp/icons");
		CHECK(wt2.save(&err));
		CHECK(slurp(path).contains("flavor_icons = \"/tmp/icons\"\n"));
	}

	/* A missing file: the window starts from rei-wm's default config. */
	MainWindow w4(dir.path() + "/new/config.toml", wm);
	CHECK(field<QSpinBox>(w4, "layout.gap_outer")->value() == 10);
	field<QSpinBox>(w4, "layout.gap_outer")->setValue(12);
	CHECK(w4.save(&err));
	QString created = slurp(dir.path() + "/new/config.toml");
	CHECK(created.contains("gap_outer = 12\n"));
	CHECK(created.contains("# rei-wm configuration"));  /* the default file's comments came along */

	puts("settings window: ok");
	return 0;
}
