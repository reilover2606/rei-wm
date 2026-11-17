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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include <QToolButton>

#include "colorbutton.h"
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

	/* The Wallpaper tab: groups of pictures, one "any" group, groups for particular workspaces. */
	{
		QString wpath = dir.path() + "/wall.toml";
		QDir().mkpath(dir.path() + "/pics/folder");
		for (const char *n : { "pics/one.png", "pics/two.png", "pics/folder/a.png", "pics/folder/b.jpg" }) {
			QFile pf(dir.path() + "/" + n);
			CHECK(pf.open(QIODevice::WriteOnly));
			pf.write("x");
		}
		{
			QFile wf(wpath);
			CHECK(wf.open(QIODevice::WriteOnly));
			wf.write(QString("# my wallpapers\n[wallpaper]\nmode = \"fit\"\n\n"
					 "[[wallpaper_group]]\nname = \"Forest\"\npaths = [\"%1/pics/folder\"]\ninterval = 120\nany = true\n\n"
					 "[[wallpaper_group]]\nname = \"Desk\"\npaths = [\n    \"%1/pics/one.png\",   # the first\n    \"%1/pics/two.png\",\n]\nworkspaces = [2, 5]\n")
					 .arg(dir.path()).toUtf8());
		}
		MainWindow ww(wpath, wm);
		auto *tabs = ww.findChild<QTabWidget *>("tabs");
		bool has_tab = false, has_look = false;
		for (int i = 0; i < tabs->count(); i++) {
			has_tab |= tabs->tabText(i) == "Wallpaper";
			has_look |= tabs->tabText(i) == "Look";
		}
		CHECK(has_tab && !has_look);   /* (it used to be called Look) */
		auto *table = field<QTableWidget>(ww, "wallpaper_groups");
		auto any = [&](int r) { return table->cellWidget(r, 3)->findChild<QRadioButton *>()->isChecked(); };
		auto spin = [&](int r) { return qobject_cast<QSpinBox *>(table->cellWidget(r, 2))->value(); };
		CHECK(table->rowCount() == 2);
		CHECK(table->item(0, 0)->text() == "Forest" && any(0) && !any(1) && spin(0) == 120 && spin(1) == 600);
		CHECK(table->item(0, 1)->text().contains("Folder folder") && table->item(0, 1)->text().contains("2"));
		CHECK(table->item(1, 1)->text().contains("2 picture"));   /* the multi-line array was read */
		CHECK(table->item(1, 4)->text() == "2, 5");
		CHECK(field<QComboBox>(ww, "wallpaper.mode")->currentText() == "fit");
		CHECK(!ww.dirty());
		QString before = slurp(wpath);
		CHECK(ww.save(&err));
		CHECK(slurp(wpath) == before);   /* opening and saving changes nothing */

		/* One picture is a group of one; several pictures, or a folder, are a group of those. */
		ww.addWallpapers({ dir.path() + "/pics/one.png" });
		CHECK(table->rowCount() == 3 && table->item(2, 0)->text() == "one" && table->item(2, 1)->text() == "one.png");
		ww.addWallpapers({ dir.path() + "/pics/one.png", dir.path() + "/pics/two.png" });
		CHECK(table->item(3, 1)->text().contains("2 picture") && table->item(3, 0)->text() == "pics");
		ww.addWallpapers({ dir.path() + "/pics/folder" });
		CHECK(table->item(4, 0)->text() == "folder" && table->item(4, 1)->text().contains("2 picture"));
		CHECK(ww.dirty());
		/* Only one group can be the any group: choosing another one moves it. */
		table->cellWidget(2, 3)->findChild<QRadioButton *>()->setChecked(true);
		CHECK(!any(0) && any(2));
		qobject_cast<QSpinBox *>(table->cellWidget(3, 2))->setValue(45);
		table->item(3, 4)->setText("3");
		table->item(2, 4)->setText("1");
		CHECK(ww.save(&err));
		QString wt = slurp(wpath);
		CHECK(wt.startsWith("# my wallpapers\n"));   /* the comment is still there */
		CHECK(wt.contains("name = \"one\"\npaths = [\"" + dir.path() + "/pics/one.png\"]\nany = true\nworkspaces = [1]\n"));
		CHECK(wt.contains("interval = 45\nworkspaces = [3]\n"));
		CHECK(!wt.contains("# the first"));   /* the Desk group was rewritten with the rest */
		{
			MainWindow again(wpath, wm);   /* and it reads back the same */
			auto *t2 = field<QTableWidget>(again, "wallpaper_groups");
			CHECK(t2->rowCount() == 5 && t2->item(2, 0)->text() == "one" && t2->item(2, 4)->text() == "1");
			CHECK(t2->cellWidget(2, 3)->findChild<QRadioButton *>()->isChecked() && !t2->cellWidget(0, 3)->findChild<QRadioButton *>()->isChecked());
			CHECK(qobject_cast<QSpinBox *>(t2->cellWidget(3, 2))->value() == 45);
		}

		/* A workspace for two groups, or something that is not a workspace, is refused, with the file left alone. */
		QString saved = slurp(wpath);
		table->item(4, 4)->setText("3");   /* the "pics" group has it */
		QString problem;
		CHECK(!ww.save(&problem) && problem.contains("Workspace 3 is given to two groups"));
		table->item(4, 4)->setText("two");
		CHECK(!ww.save(&problem) && problem.contains("must be numbers from 1 to 99"));
		table->item(4, 4)->setText("100");
		CHECK(!ww.save(&problem) && problem.contains("1 to 99"));
		CHECK(slurp(wpath) == saved);
		table->item(4, 4)->setText("");

		/* No any group, and removing groups. */
		field<QPushButton>(ww, "wallpaper_no_any")->click();
		CHECK(!any(2));
		table->selectRow(4);
		field<QPushButton>(ww, "wallpaper_remove")->click();
		CHECK(table->rowCount() == 4);
		CHECK(ww.save(&err));
		CHECK(!slurp(wpath).contains("any = true"));
		CHECK(slurp(wpath).count("[[wallpaper_group]]") == 4);
		/* All the groups removed: no [[wallpaper_group]] left. */
		table->selectAll();
		field<QPushButton>(ww, "wallpaper_remove")->click();
		CHECK(table->rowCount() == 0);
		CHECK(ww.save(&err));
		CHECK(!slurp(wpath).contains("wallpaper_group") && slurp(wpath).contains("mode = \"fit\""));

		/* The old single [wallpaper] path shows as a group that shows everywhere; saving as it is leaves the file alone. */
		QString lpath = dir.path() + "/legacy.toml";
		{
			QFile lf(lpath);
			CHECK(lf.open(QIODevice::WriteOnly));
			lf.write(QString("[wallpaper]\npath = \"%1/pics/one.png\"\nmode = \"fill\"\n").arg(dir.path()).toUtf8());
		}
		MainWindow lw(lpath, wm);
		auto *lt = field<QTableWidget>(lw, "wallpaper_groups");
		CHECK(lt->rowCount() == 1 && lt->item(0, 0)->text() == "one" && lt->cellWidget(0, 3)->findChild<QRadioButton *>()->isChecked());
		QString lbefore = slurp(lpath);
		CHECK(!lw.dirty() && lw.save(&err) && slurp(lpath) == lbefore);
		lw.addWallpapers({ dir.path() + "/pics/folder" });
		CHECK(lw.save(&err));
		QString lt2 = slurp(lpath);
		CHECK(!lt2.contains("path = \"" + dir.path() + "/pics/one.png\"\nmode") && lt2.count("[[wallpaper_group]]") == 2);
		CHECK(lt2.contains("name = \"one\"\npaths = [\"" + dir.path() + "/pics/one.png\"]\nany = true\n"));
	}

	/* Wifi and brightness settings. */
	{
		MainWindow ww(path, wm);
		CHECK(field<QCheckBox>(ww, "toolbar.wifi")->isChecked());
		CHECK(field<QWidget>(ww, "toolbar.wifi_command")->findChild<QLineEdit *>("edit")->text() == "wpa_gui");
		CHECK(field<QSpinBox>(ww, "brightness.step")->value() == 5);
		field<QCheckBox>(ww, "toolbar.wifi")->setChecked(false);
		field<QSpinBox>(ww, "brightness.step")->setValue(8);
		field<QWidget>(ww, "brightness.command")->findChild<QLineEdit *>("edit")->setText("brightnessctl set {percent}%");
		CHECK(ww.save(&err));
		QString wt = slurp(path);
		CHECK(wt.contains("wifi = false\n") && wt.contains("[brightness]\nstep = 8\n"));
		CHECK(wt.contains("command = \"brightnessctl set {percent}%\"\n"));
		CHECK(ww.findChild<QWidget *>("theme.wifi.inactive_color") != nullptr);
	}

	/* The music tab and the message view's blur. */
	{
		MainWindow wm2(path, wm);
		CHECK(field<QCheckBox>(wm2, "mpd.enabled")->isChecked() && field<QCheckBox>(wm2, "mpd.announce")->isChecked());
		CHECK(field<QSpinBox>(wm2, "mpd.port")->value() == 6600);
		CHECK(field<QWidget>(wm2, "mpd.start_command")->findChild<QLineEdit *>("edit")->text() == "mpd");
		CHECK(field<QSpinBox>(wm2, "messages.blur_radius")->value() == 12);
		field<QCheckBox>(wm2, "mpd.announce")->setChecked(false);
		field<QWidget>(wm2, "mpd.host")->findChild<QLineEdit *>("edit")->setText("localhost");
		field<QSpinBox>(wm2, "messages.blur_radius")->setValue(20);
		CHECK(wm2.save(&err));
		QString mt = slurp(path);
		CHECK(mt.contains("[mpd]\nannounce = false\nhost = \"localhost\"\n"));
		CHECK(mt.contains("blur_radius = 20\n"));
		CHECK(wm2.findChild<QWidget *>("theme.mpd.color") != nullptr);   /* and a page in the theme editor */
	}

	/* The cursor tab: the installed themes (here two made-up ones, found through XCURSOR_PATH), a preview drawn by
	 * rei-wm, and the choice written as [cursor] theme / size. */
	{
		QTemporaryDir themes;
		CHECK(themes.isValid());
		CHECK(QDir().mkpath(themes.path() + "/alpha/cursors") && QDir().mkpath(themes.path() + "/beta/cursors"));
		CHECK(QDir().mkpath(themes.path() + "/not-a-cursor-theme/icons"));
		qputenv("XCURSOR_PATH", themes.path().toUtf8());
		MainWindow wc(path, wm);
		auto *theme = field<QComboBox>(wc, "cursor.theme");
		CHECK(theme->findText("alpha") >= 0 && theme->findText("beta") >= 0);
		CHECK(theme->findText("not-a-cursor-theme") < 0);
		CHECK(theme->currentText().isEmpty() && field<QSpinBox>(wc, "cursor.size")->value() == 24);   /* the defaults */
		CHECK(!wc.dirty());
		theme->setCurrentText("beta");
		field<QSpinBox>(wc, "cursor.size")->setValue(32);
		CHECK(wc.dirty());
		auto *preview = field<QLabel>(wc, "cursor.preview");
		CHECK(!preview->pixmap().isNull());   /* the PNG rei-wm drew, which has the pointers of two grounds */
		CHECK(wc.save(&err));
		QString t = slurp(path);
		CHECK(t.contains("[cursor]\ntheme = \"beta\"\nsize = 32\n"));
		/* And back: a theme the list does not know is kept as typed, not reset. */
		MainWindow wc2(path, wm);
		CHECK(field<QComboBox>(wc2, "cursor.theme")->currentText() == "beta");
		field<QComboBox>(wc2, "cursor.theme")->setCurrentText("elsewhere");
		CHECK(wc2.save(&err));
		MainWindow wc3(path, wm);
		CHECK(field<QComboBox>(wc3, "cursor.theme")->currentText() == "elsewhere");
		qunsetenv("XCURSOR_PATH");
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
		/* Text effects are part of every element's page; terminal windows have a page of their own. */
		CHECK(field<QDoubleSpinBox>(wt2, "theme.toolbar.text_glow_size")->value() == 0);
		CHECK(field<QDoubleSpinBox>(wt2, "theme.toolbar.text_glow_strength")->value() == 1);
		CHECK(field<QLineEdit>(wt2, "theme.window_text.apps")->text() == "URxvt");
		CHECK(field<QLineEdit>(wt2, "theme.window_text.shader")->text() == "glow");
		CHECK(field<QDoubleSpinBox>(wt2, "theme.window_text.text_glow_size")->value() == 5);
		CHECK(wt2.findChild<QWidget *>("theme.window_text.color") == nullptr);        /* nothing else applies to it */
		CHECK(wt2.findChild<QWidget *>("theme.window_text.text_shine") == nullptr);
		field<QDoubleSpinBox>(wt2, "theme.toolbar.text_glow_size")->setValue(6);
		field<QDoubleSpinBox>(wt2, "theme.run_prompt.text_shine")->setValue(0.8);
		field<QDoubleSpinBox>(wt2, "theme.tray_tile.text_blink")->setValue(1.5);
		/* (found as a plain widget: ColorButton has no Q_OBJECT, which newer Qt requires of findChild's type) */
		static_cast<ColorButton *>(field<QWidget>(wt2, "theme.toolbar.text_glow_color"))->setColor("#ff8800");
		field<QLineEdit>(wt2, "theme.window_text.apps")->setText("URxvt, foot");
		field<QDoubleSpinBox>(wt2, "theme.window_text.glow_threshold")->setValue(0.5);
		CHECK(field<QDoubleSpinBox>(wt2, "theme.window_text.glow_area_limit")->value() == 0.5);
		CHECK(wt2.dirty());
		CHECK(wt2.save(&err));
		QString fxText = slurp(path);
		CHECK(fxText.contains("text_glow_size = 6.0\n") && fxText.contains("text_glow_color = \"#ff8800\"\n"));
		CHECK(fxText.contains("[theme.run_prompt]\ntext_shine = 0.8\n"));
		CHECK(fxText.contains("text_blink = 1.5\n"));
		CHECK(fxText.contains("[theme.window_text]\napps = \"URxvt, foot\"\nglow_threshold = 0.5\n"));
		{
			MainWindow wt3(path, wm);
			CHECK(field<QLineEdit>(wt3, "theme.window_text.apps")->text() == "URxvt, foot");
			CHECK(field<QDoubleSpinBox>(wt3, "theme.run_prompt.text_shine")->value() == 0.8);
			/* Back to the built-in value removes the line again. */
			field<QLineEdit>(wt3, "theme.window_text.apps")->setText("URxvt");
			CHECK(wt3.save(&err));
			CHECK(!slurp(path).contains("apps ="));
		}
		/* The flavor icons folder is a plain [theme] setting. */
		field<QWidget>(wt2, "theme.flavor_icons")->findChild<QLineEdit *>("edit")->setText("/tmp/icons");
		CHECK(wt2.save(&err));
		CHECK(slurp(path).contains("flavor_icons = \"/tmp/icons\"\n"));
	}

	/* Reset buttons, and base colours the built-in look follows. */
	{
		QString rpath = dir.path() + "/reset.toml";
		{
			QFile f(rpath);
			CHECK(f.open(QIODevice::WriteOnly));
			f.write("[toolbar]\nenabled = true\n\n"
				"[theme.tray_tile]\nradius = 9\nshape = \"pill\"\n\n"
				"[theme.dialog]\nradius = 20\n");
		}
		MainWindow wr(rpath, wm);
		auto *radius = field<QDoubleSpinBox>(wr, "theme.tray_tile.radius");
		CHECK(radius->value() == 9);
		auto *resetRadius = wr.findChild<QToolButton *>("reset.tray_tile.radius");
		CHECK(resetRadius && resetRadius->isEnabled());                       /* it differs from the built-in 4 */
		auto *untouched = wr.findChild<QToolButton *>("reset.tray_tile.opacity");
		CHECK(untouched && !untouched->isEnabled());                          /* already the built-in value */

		/* Resetting an overridden value removes the override from the file (it does not pin the default). */
		resetRadius->click();
		CHECK(radius->value() == 4);
		CHECK(!resetRadius->isEnabled());
		CHECK(wr.dirty());
		CHECK(wr.save(&err));
		QString r1 = slurp(rpath);
		CHECK(!r1.contains("radius = 9"));
		CHECK(r1.contains("[theme.tray_tile]\nshape = \"pill\"\n"));          /* the other override stays */

		/* Resetting a whole element drops its table altogether. */
		wr.findChild<QPushButton *>("reset_element.dialog")->click();
		CHECK(field<QDoubleSpinBox>(wr, "theme.dialog.radius")->value() == 12);
		CHECK(wr.save(&err));
		QString r2 = slurp(rpath);
		CHECK(!r2.contains("[theme.dialog]"));
		CHECK(r2.contains("[theme.tray_tile]") && r2.contains("[toolbar]\nenabled = true\n"));

		/* Reset everything. */
		wr.findChild<QPushButton *>("theme_reset_all")->click();
		CHECK(field<QComboBox>(wr, "theme.tray_tile.shape")->currentText() == "rounded");
		CHECK(wr.save(&err));
		CHECK(!slurp(rpath).contains("[theme."));

		/* The three base colours live on the Theme tab; the built-in look follows them. */
		MainWindow wb(rpath, wm);
		auto *bg = static_cast<ColorButton *>(field<QWidget>(wb, "toolbar.background"));
		QString oldColor = field<QWidget>(wb, "theme.menu.color")->property("text").toString();
		auto *menuColor = static_cast<ColorButton *>(field<QWidget>(wb, "theme.menu.color"));
		oldColor = menuColor->color();
		bg->setColor("#402010");
		bg->onChange();
		CHECK(menuColor->color() != oldColor);                                 /* followed the new background */
		CHECK(wb.dirty());
		/* A value set by hand does not follow. */
		field<QDoubleSpinBox>(wb, "theme.menu.radius")->setValue(7);
		bg->setColor("#103040");
		bg->onChange();
		CHECK(field<QDoubleSpinBox>(wb, "theme.menu.radius")->value() == 7);
		CHECK(wb.save(&err));
		QString r3 = slurp(rpath);
		CHECK(r3.contains("background = \"#103040\""));
		CHECK(r3.contains("[theme.menu]\nradius = 7.0\n"));
		CHECK(!r3.contains("[theme.menu]\ncolor"));                           /* followed values are not pinned */
		MainWindow wb2(rpath, wm);
		CHECK(field<QDoubleSpinBox>(wb2, "theme.menu.radius")->value() == 7);
		CHECK(static_cast<ColorButton *>(field<QWidget>(wb2, "theme.menu.color"))->color() == menuColor->color());
	}

	/* If rei-wm cannot be asked, the Theme tab says so and offers to locate it, instead of silently
	 * graying out. (A working one hides the notice.) */
	{
		MainWindow wbad(path, "/nonexistent/rei-wm");
		auto *box = wbad.findChild<QWidget *>("theme_notice_box");
		CHECK(box && !box->isHidden());
		CHECK(!wbad.findChild<QLabel *>("theme_notice")->text().isEmpty());
		CHECK(wbad.findChild<QPushButton *>("theme_locate") != nullptr);
		MainWindow wgood(path, wm);
		CHECK(wgood.findChild<QWidget *>("theme_notice_box")->isHidden());
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
