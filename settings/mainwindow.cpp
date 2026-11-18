#include "mainwindow.h"

#include "colorbutton.h"
#include "themeeditor.h"

#include <QApplication>
#include <QButtonGroup>
#include <QRadioButton>
#include <QRegularExpression>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFontDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QVBoxLayout>
#include <functional>


MainWindow::MainWindow(const QString &configPath, const QString &compositorBinary, QWidget *parent)
	: QMainWindow(parent), path_(configPath), binary_(compositorBinary)
{
	setWindowTitle(tr("rei-wm settings"));
	resize(720, 560);

	auto *central = new QWidget;
	auto *outer = new QVBoxLayout(central);
	auto *tabs = new QTabWidget;
	tabs->setObjectName("tabs");
	tabs->addTab(buildGeneralTab(), tr("General"));
	tabs->addTab(buildLayoutTab(), tr("Windows"));
	tabs->addTab(buildInputTab(), tr("Input"));
	tabs->addTab(buildToolbarTab(), tr("Toolbar"));
	tabs->addTab(buildWallpaperTab(), tr("Wallpaper"));
	tabs->addTab(buildCursorTab(), tr("Cursor"));
	tabs->addTab(buildStartMenuTab(), tr("Start menu"));
	tabs->addTab(buildKeysTab(), tr("Keybindings"));
	tabs->addTab(buildOutputsTab(), tr("Monitors"));
	tabs->addTab(buildAudioTab(), tr("Audio"));
	tabs->addTab(buildMessagesTab(), tr("Messages"));
	tabs->addTab(buildMusicTab(), tr("Music"));
	tabs->addTab(buildThemeTab(), tr("Theme"));
	outer->addWidget(tabs);

	auto *bottom = new QHBoxLayout;
	status_ = new QLabel;
	status_->setObjectName("status");
	status_->setWordWrap(true);
	bottom->addWidget(status_, 1);
	auto *revert = new QPushButton(tr("Revert"));
	revert->setObjectName("revert");
	auto *save = new QPushButton(tr("Save"));
	save->setObjectName("save");
	save->setDefault(true);
	bottom->addWidget(revert);
	bottom->addWidget(save);
	outer->addLayout(bottom);
	setCentralWidget(central);

	connect(save, &QPushButton::clicked, this, &MainWindow::onSaveClicked);
	connect(revert, &QPushButton::clicked, this, &MainWindow::onRevertClicked);

	QString err;
	if (!load(&err))
		status_->setText(err);
}

/* ------------------------------------------------------------------------ */
/* Fields                                                                   */
/* ------------------------------------------------------------------------ */

QWidget *MainWindow::addField(QFormLayout *form, const QString &label, const QString &section,
			      const QString &key, Type type, const QVariant &fallback,
			      const QStringList &choices, int min, int max, double step,
			      const QString &tooltip)
{
	QWidget *w = nullptr;
	switch (type) {
	case Bool: {
		auto *c = new QCheckBox;
		connect(c, &QCheckBox::toggled, this, &MainWindow::markDirty);
		w = c;
		break;
	}
	case Int: {
		auto *s = new QSpinBox;
		s->setRange(min, max);
		connect(s, &QSpinBox::valueChanged, this, &MainWindow::markDirty);
		w = s;
		break;
	}
	case Double: {
		auto *s = new QDoubleSpinBox;
		s->setRange(min, max);
		s->setSingleStep(step);
		s->setDecimals(2);
		connect(s, &QDoubleSpinBox::valueChanged, this, &MainWindow::markDirty);
		w = s;
		break;
	}
	case Choice: {
		auto *c = new QComboBox;
		c->addItems(choices);
		connect(c, &QComboBox::currentTextChanged, this, &MainWindow::markDirty);
		w = c;
		break;
	}
	case Color: {
		auto *b = new ColorButton;
		b->onChange = [this] { markDirty(); };
		w = b;
		break;
	}
	case Path:
	case Dir:
	case Font:
	case String: {
		auto *holder = new QWidget;
		auto *h = new QHBoxLayout(holder);
		h->setContentsMargins(0, 0, 0, 0);
		auto *e = new QLineEdit;
		e->setObjectName("edit");
		h->addWidget(e, 1);
		connect(e, &QLineEdit::textChanged, this, &MainWindow::markDirty);
		if (type == Path) {
			auto *b = new QPushButton(tr("Browse…"));
			h->addWidget(b);
			connect(b, &QPushButton::clicked, this, [this, e] {
				QString f = QFileDialog::getOpenFileName(this, tr("Choose an image"), e->text(),
									 tr("Images (*.png *.jpg *.jpeg *.bmp *.tga)"));
				if (!f.isEmpty())
					e->setText(f);
			});
		} else if (type == Dir) {
			auto *b = new QPushButton(tr("Browse…"));
			h->addWidget(b);
			connect(b, &QPushButton::clicked, this, [this, e] {
				QString d = QFileDialog::getExistingDirectory(this, tr("Choose a folder"), e->text());
				if (!d.isEmpty())
					e->setText(d);
			});
		} else if (type == Font) {
			auto *b = new QPushButton(tr("Choose…"));
			h->addWidget(b);
			connect(b, &QPushButton::clicked, this, [this, e] {
				bool ok = false;
				QFont f = QFontDialog::getFont(&ok, QFont(), this);
				if (ok)
					e->setText(QString("%1 %2").arg(f.family()).arg(f.pointSize() > 0 ? f.pointSize() : 11));
			});
		}
		w = holder;
		break;
	}
	}
	w->setObjectName(section + "." + key);
	if (!tooltip.isEmpty())
		w->setToolTip(tooltip);
	form->addRow(label, w);
	fields_.append({ section, key, type, w, fallback, QVariant() });
	return w;
}

QVariant MainWindow::fieldValue(const Field &f) const
{
	switch (f.type) {
	case Bool: return static_cast<QCheckBox *>(f.widget)->isChecked();
	case Int: return (qlonglong)static_cast<QSpinBox *>(f.widget)->value();
	case Double: return static_cast<QDoubleSpinBox *>(f.widget)->value();
	case Choice: return static_cast<QComboBox *>(f.widget)->currentText();
	case Color: return static_cast<ColorButton *>(f.widget)->color();
	default: return f.widget->findChild<QLineEdit *>("edit")->text();
	}
}

void MainWindow::setFieldValue(Field &f, const QVariant &v)
{
	switch (f.type) {
	case Bool: static_cast<QCheckBox *>(f.widget)->setChecked(v.toBool()); break;
	case Int: static_cast<QSpinBox *>(f.widget)->setValue(v.toInt()); break;
	case Double: static_cast<QDoubleSpinBox *>(f.widget)->setValue(v.toDouble()); break;
	case Choice: {
		auto *c = static_cast<QComboBox *>(f.widget);
		int i = c->findText(v.toString());
		if (i >= 0)
			c->setCurrentIndex(i);
		else if (c->isEditable())
			c->setEditText(v.toString());   /* a theme that is installed somewhere the list does not look */
		else
			c->setCurrentIndex(0);
		break;
	}
	case Color: static_cast<ColorButton *>(f.widget)->setColor(v.toString()); break;
	default: f.widget->findChild<QLineEdit *>("edit")->setText(v.toString()); break;
	}
	f.loaded = fieldValue(f);  /* What the widget normalised it to. */
}

/* ------------------------------------------------------------------------ */
/* Tabs                                                                     */
/* ------------------------------------------------------------------------ */

QWidget *MainWindow::buildGeneralTab()
{
	auto *w = new QWidget;
	auto *f = new QFormLayout(w);
	addField(f, tr("Backend"), "general", "backend", Choice, "auto", { "auto", "drm", "headless", "nested" },
		 0, 0, 1, tr("Needs a restart of rei-wm to change."));
	addField(f, tr("Composition key (“Mod”)"), "general", "composition_key", Choice, "super",
		 { "super", "alt", "ctrl", "shift" });
	addField(f, tr("Focus follows the mouse"), "general", "focus_follows_mouse", Bool, true, {}, 0, 0, 1,
		 tr("Hovering a window focuses it; leaving every window focuses nothing. Off: click to focus."));
	addField(f, tr("Turn the display off after (minutes)"), "general", "display_sleep", Int, 5, {}, 0, 1440, 1,
		 tr("Without keyboard or mouse use. Any input wakes it. 0 means never. Programs playing video keep it awake."));
	addField(f, tr("Highlighted text in a terminal is also the clipboard"), "clipboard", "terminal_selection_to_clipboard",
		 Bool, true, {}, 0, 0, 1,
		 tr("Terminals copy by highlighting (paste with a middle click). With this on, what you highlight in one of the "
		    "terminals below can also be pasted with Ctrl+V in other programs."));
	addField(f, tr("Terminals (app names, comma separated)"), "clipboard", "terminals", String, "URxvt, XTerm, foot, kitty, Alacritty",
		 {}, 0, 0, 1, tr("Windows of these programs count as terminals. Case does not matter; the X11 class or Wayland app id."));
	addField(f, tr("Run Xwayland (X11 programs)"), "general", "xwayland", Bool, true,
		 {}, 0, 0, 1, tr("Needs a restart of rei-wm to change."));
	addField(f, tr("X resources file"), "general", "xresources", String, "~/.Xresources", {}, 0, 0, 1,
		 tr("Loaded into Xwayland with xrdb when it starts (colours, fonts and transparency of urxvt, xterm…). "
		    "Empty turns it off."));
	addField(f, tr("Session bus for launched programs"), "general", "session_bus", Choice, "inherit",
		 { "inherit", "private" }, 0, 0, 1,
		 tr("\"private\" gives programs started from rei-wm their own D-Bus session bus, so single-instance GTK "
		    "programs do not open in another desktop of yours (it has no portals, notifications or keyring). "
		    "Needs a restart."));
	addField(f, tr("Volume control (PipeWire)"), "audio", "enabled", Bool, true, {}, 0, 0, 1,
		 tr("The toolbar's volume text and the media keys. Needs a restart to change."));
	addField(f, tr("Volume step (percent)"), "audio", "volume_step", Int, 5, {}, 1, 100);
	addField(f, tr("What the volume keys control"), "audio", "volume_target", Choice, "auto", { "auto", "sink", "mpd", "both" }, 0, 0, 1,
		 tr("MPD has a volume of its own. auto: MPD's while it is playing (shown as ♪ 60%), the sound system's otherwise; "
		    "sink: always the sound system's; mpd: MPD's whenever it runs; both: the sound system's, with MPD set to match."));
	addField(f, tr("Maximum volume (percent)"), "audio", "max_volume", Int, 100, {}, 10, 150, 1,
		 tr("Above 100 amplifies and may distort."));
	return w;
}

QWidget *MainWindow::buildLayoutTab()
{
	auto *w = new QWidget;
	auto *f = new QFormLayout(w);
	addField(f, tr("New windows"), "layout", "new_windows", Choice, "tile", { "tile", "float" });
	addField(f, tr("Outer gap (px)"), "layout", "gap_outer", Int, 10, {}, 0, 500);
	addField(f, tr("Inner gap (px)"), "layout", "gap_inner", Int, 10, {}, 0, 500);
	addField(f, tr("Show programs' own title bars"), "decoration", "client_titlebars", Bool, true, {}, 0, 0, 1,
		 tr("Off: programs that honour the xdg-decoration protocol (Qt, SDL, GTK4, many others) leave their title bar out. "
		    "Programs that never ask (GTK3, Firefox's own bar) and X11 programs are not affected."));
	addField(f, tr("Window notch height (px, 0 = off)"), "decoration", "notch_height", Int, 22, {}, 0, 64);
	return w;
}

QWidget *MainWindow::buildInputTab()
{
	auto *w = new QWidget;
	auto *f = new QFormLayout(w);
	addField(f, tr("Keyboard layout"), "input", "keyboard_layout", String, "", {}, 0, 0, 1,
		 tr("xkb layout names, e.g. us or us,de. Empty uses the system default."));
	addField(f, tr("Layout variant"), "input", "keyboard_variant", String, "");
	addField(f, tr("Keyboard options"), "input", "keyboard_options", String, "", {}, 0, 0, 1,
		 tr("e.g. ctrl:nocaps"));
	addField(f, tr("Keyboard model"), "input", "keyboard_model", String, "");
	addField(f, tr("Keyboard rules"), "input", "keyboard_rules", String, "");
	addField(f, tr("Key repeat rate (per second, 0 = off)"), "input", "repeat_rate", Int, 25, {}, 0, 1000);
	addField(f, tr("Key repeat delay (ms)"), "input", "repeat_delay", Int, 600, {}, 0, 10000);
	addField(f, tr("Tap to click"), "input", "tap_to_click", Bool, true);
	addField(f, tr("Natural scrolling"), "input", "natural_scroll", Bool, false);
	addField(f, tr("Left-handed mouse"), "input", "left_handed", Bool, false);
	addField(f, tr("Pointer speed (-1 … 1)"), "input", "pointer_accel", Double, 0.0, {}, -1, 1, 0.1);
	addField(f, tr("Brightness step (percent per key press)"), "brightness", "step", Int, 5, {}, 1, 50, 1,
		 tr("For the brightness keys (XF86MonBrightnessUp/Down). The steps are finer at the dim end, and the screen "
		    "never goes below 1%."));
	addField(f, tr("Backlight device"), "brightness", "device", String, "", {}, 0, 0, 1,
		 tr("A name under /sys/class/backlight; empty: the finest one."));
	addField(f, tr("Brightness command"), "brightness", "command", String, "", {}, 0, 0, 1,
		 tr("Your own command instead of writing the backlight (or asking logind to), e.g. “brightnessctl set {percent}%”. "
		    "{value}, {percent} and {device} are filled in."));
	return w;
}

QWidget *MainWindow::buildToolbarTab()
{
	auto *w = new QWidget;
	auto *f = new QFormLayout(w);
	addField(f, tr("Show the toolbar"), "toolbar", "enabled", Bool, true);
	addField(f, tr("Position"), "toolbar", "position", Choice, "top", { "top", "bottom", "left", "right" });
	addField(f, tr("Thickness (px)"), "toolbar", "thickness", Int, 28, {}, 16, 96);
	addField(f, tr("Application tray"), "toolbar", "tray", Choice, "icon_and_name",
		 { "icon_and_name", "icon", "off" }, 0, 0, 1,
		 tr("The running windows, one tile per application, in the middle of the bar."));
	addField(f, tr("Show the battery"), "toolbar", "battery", Bool, true, {}, 0, 0, 1,
		 tr("An icon and percentage before the volume and clock. Appears only if the machine has a battery; "
		    "several batteries are combined."));
	addField(f, tr("Show the battery percentage"), "toolbar", "battery_percent", Bool, true, {}, 0, 0, 1,
		 tr("Off: only the icon is shown, and hovering it shows the exact percentage."));
	addField(f, tr("Show the wifi icon"), "toolbar", "wifi", Bool, true, {}, 0, 0, 1,
		 tr("A small signal icon (red and struck through when not connected). Appears only if the machine has a "
		    "wireless interface."));
	addField(f, tr("Command the wifi icon runs"), "toolbar", "wifi_command", String, "wpa_gui", {}, 0, 0, 1,
		 tr("Run when the icon is clicked, e.g. wpa_gui, nm-connection-editor, or a terminal with iwctl."));
	addField(f, tr("Show CPU and RAM use"), "toolbar", "sysinfo", Bool, true, {}, 0, 0, 1,
		 tr("One box left of the clock: the busy share of all cores and memory used/total, every two seconds."));
	addField(f, tr("Tray lists"), "toolbar", "tray_scope", Choice, "workspace", { "workspace", "output" }, 0, 0, 1,
		 tr("This monitor's current workspace, or all of its workspaces. With \"workspace\", the 33rd window "
		    "in a full tray opens on the next workspace."));
	addField(f, tr("Tray tiles shrink to (percent)"), "toolbar", "tray_min_width", Int, 50, {}, 10, 100, 1,
		 tr("Tiles shrink this far as the tray fills; after that new windows go under a \"+N\" tile."));
	addField(f, tr("Font"), "toolbar", "font", Font, "Sans 11", {}, 0, 0, 1,
		 tr("A Pango font description, e.g. “Sans 11”."));
	addField(f, tr("Clock format"), "toolbar", "clock_format", String, "%H:%M:%S", {}, 0, 0, 1,
		 tr("strftime format; empty hides the clock."));
	auto *colorsNote = new QLabel(tr("The toolbar's three base colours are on the Theme tab."));
	f->addRow(colorsNote);
	return w;
}

static QTableWidget *makeTable(const QStringList &headers);

/* ---- The wallpaper tab: groups of pictures ---- */

static const char *kImageFilter = "*.png *.jpg *.jpeg *.bmp *.tga *.gif";
enum { WpName, WpPictures, WpInterval, WpAny, WpWorkspaces, WpColumns };

/* How many pictures a path stands for: a folder's images, or the file itself. */
static int pictureCount(const QString &path)
{
	QFileInfo fi(path);
	if (fi.isDir())
		return (int)QDir(path).entryList(QString(kImageFilter).split(' '), QDir::Files).size();
	return fi.exists() ? 1 : 0;
}

static QString pictures(int n)
{
	return n == 1 ? QObject::tr("1 picture") : QObject::tr("%1 pictures").arg(n);
}

static QString summaryOf(const QStringList &paths)
{
	if (paths.size() == 1) {
		QFileInfo fi(paths[0]);
		if (fi.isDir()) {
			int n = pictureCount(paths[0]);
			return QObject::tr("Folder %1 (%2)").arg(fi.fileName().isEmpty() ? paths[0] : fi.fileName(), pictures(n));
		}
		return fi.fileName();
	}
	int total = 0;
	for (const QString &p : paths)
		total += pictureCount(p);
	return pictures(total);
}

QWidget *MainWindow::buildWallpaperTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *hint = new QLabel(tr("A wallpaper group is one picture, or many that take turns. Choose which group shows on every workspace "
				   "(the “any” column; only one group can be it), and give a group to particular workspaces if they "
				   "should look different (write their numbers, like 2, 5). A workspace with no group of its own shows "
				   "the “any” group. Add a single picture to make a group of one; add several pictures, or a folder, "
				   "for a group that changes by itself."));
	hint->setWordWrap(true);
	v->addWidget(hint);
	auto *f = new QFormLayout;
	addField(f, tr("Fit"), "wallpaper", "mode", Choice, "fill", { "fill", "fit", "stretch", "center", "tile" });
	addField(f, tr("Background colour"), "wallpaper", "color", Color, "#1b2733");
	v->addLayout(f);

	wallpapers_ = makeTable({ tr("Group"), tr("Wallpapers"), tr("Change every"), tr("Any"), tr("Workspaces") });
	wallpapers_->setObjectName("wallpaper_groups");
	wallpapers_->horizontalHeader()->setSectionResizeMode(WpPictures, QHeaderView::Stretch);
	wallpapers_->horizontalHeader()->setStretchLastSection(false);
	wallpapers_->horizontalHeader()->setSectionResizeMode(WpWorkspaces, QHeaderView::ResizeToContents);
	v->addWidget(wallpapers_, 1);
	anyGroup_ = new QButtonGroup(this);
	anyGroup_->setExclusive(true);

	auto *row = new QHBoxLayout;
	auto *addImages = new QPushButton(tr("Add picture(s)…"));
	addImages->setObjectName("wallpaper_add_images");
	addImages->setToolTip(tr("One picture makes a group of one; select several (Shift or Ctrl) for a group that cycles."));
	auto *addFolder = new QPushButton(tr("Add folder…"));
	addFolder->setObjectName("wallpaper_add_folder");
	addFolder->setToolTip(tr("Every picture in the folder is the group; the folder is looked at again each time it moves on."));
	auto *none = new QPushButton(tr("No “any” group"));
	none->setObjectName("wallpaper_no_any");
	none->setToolTip(tr("Workspaces without a group of their own then show the plain background colour."));
	auto *del = new QPushButton(tr("Remove"));
	del->setObjectName("wallpaper_remove");
	row->addWidget(addImages);
	row->addWidget(addFolder);
	row->addStretch(1);
	row->addWidget(none);
	row->addWidget(del);
	v->addLayout(row);

	connect(addImages, &QPushButton::clicked, this, [this] {
		QString start = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
		QStringList files = QFileDialog::getOpenFileNames(this, tr("Choose pictures"), start,
								  tr("Pictures (%1)").arg(kImageFilter));
		if (!files.isEmpty())
			addWallpapers(files);
	});
	connect(addFolder, &QPushButton::clicked, this, [this] {
		QString start = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
		QString dir = QFileDialog::getExistingDirectory(this, tr("Choose a folder of pictures"), start);
		if (!dir.isEmpty())
			addWallpapers({ dir });
	});
	connect(none, &QPushButton::clicked, this, [this] {
		anyGroup_->setExclusive(false);
		for (QAbstractButton *b : anyGroup_->buttons())
			b->setChecked(false);
		anyGroup_->setExclusive(true);
		markDirty();
	});
	connect(del, &QPushButton::clicked, this, [this] {
		auto rows = wallpapers_->selectionModel()->selectedRows();
		for (int i = rows.size() - 1; i >= 0; i--) {
			QWidget *cell = wallpapers_->cellWidget(rows[i].row(), WpAny);
			if (cell)
				for (QRadioButton *rb : cell->findChildren<QRadioButton *>())
					anyGroup_->removeButton(rb);
			wallpapers_->removeRow(rows[i].row());
		}
		markDirty();
	});
	connect(wallpapers_, &QTableWidget::itemChanged, this, &MainWindow::markDirty);
	return w;
}

void MainWindow::setWallpaperRow(int r, const QString &name, const QStringList &paths, int interval, bool any,
				 const QString &workspaces)
{
	bool was = loading_;
	loading_ = true;   /* (the cells being filled are not edits) */
	auto *nameItem = new QTableWidgetItem(name);
	wallpapers_->setItem(r, WpName, nameItem);
	auto *pics = new QTableWidgetItem(summaryOf(paths));
	pics->setFlags(pics->flags() & ~Qt::ItemIsEditable);
	pics->setData(Qt::UserRole, paths);
	pics->setToolTip(paths.join('\n'));
	wallpapers_->setItem(r, WpPictures, pics);
	auto *spin = new QSpinBox;
	spin->setRange(5, 86400);
	spin->setSuffix(tr(" s"));
	spin->setValue(interval);
	spin->setToolTip(tr("How long each picture stays, for a group with more than one. 600 s is ten minutes."));
	connect(spin, &QSpinBox::valueChanged, this, &MainWindow::markDirty);
	wallpapers_->setCellWidget(r, WpInterval, spin);
	auto *holder = new QWidget;
	auto *hl = new QHBoxLayout(holder);
	hl->setContentsMargins(0, 0, 0, 0);
	hl->setAlignment(Qt::AlignCenter);
	auto *radio = new QRadioButton;
	radio->setObjectName("wallpaper_any");
	anyGroup_->addButton(radio);
	radio->setChecked(any);
	connect(radio, &QRadioButton::toggled, this, &MainWindow::markDirty);
	hl->addWidget(radio);
	wallpapers_->setCellWidget(r, WpAny, holder);
	auto *ws = new QTableWidgetItem(workspaces);
	ws->setToolTip(tr("Workspace numbers that show this group, like 2, 5. Empty: only through the “any” group."));
	wallpapers_->setItem(r, WpWorkspaces, ws);
	loading_ = was;
}

void MainWindow::addWallpapers(const QStringList &paths)
{
	if (paths.isEmpty())
		return;
	QFileInfo first(paths[0]);
	QString name;
	if (paths.size() == 1 && first.isDir())
		name = first.fileName().isEmpty() ? paths[0] : first.fileName();
	else if (paths.size() == 1)
		name = first.completeBaseName();
	else
		name = first.dir().dirName();
	if (name.isEmpty())
		name = tr("Wallpapers");
	int r = wallpapers_->rowCount();
	wallpapers_->insertRow(r);
	setWallpaperRow(r, name, paths, 600, false, QString());
	markDirty();
}

void MainWindow::loadWallpapers()
{
	wallpapers_->setRowCount(0);
	for (QAbstractButton *b : anyGroup_->buttons())
		anyGroup_->removeButton(b);
	QList<QVariantMap> tables = doc_.tables("wallpaper_group");
	for (const QVariantMap &t : tables) {
		QStringList paths;
		for (const QVariant &p : t.value("paths").toList())
			paths << p.toString();
		if (t.contains("path"))
			paths << t.value("path").toString();
		QStringList ws;
		for (const QVariant &n : t.value("workspaces").toList())
			ws << QString::number(n.toLongLong());
		int r = wallpapers_->rowCount();
		wallpapers_->insertRow(r);
		setWallpaperRow(r, t.value("name").toString(), paths, t.contains("interval") ? t.value("interval").toInt() : 600,
				t.value("any").toBool(), ws.join(", "));
	}
	/* The old single [wallpaper] path is shown as a group of one that shows everywhere. */
	QString legacy = doc_.get("wallpaper", "path").toString();
	if (tables.isEmpty() && !legacy.isEmpty()) {
		wallpapers_->insertRow(0);
		setWallpaperRow(0, QFileInfo(legacy).completeBaseName(), { legacy }, 600, true, QString());
	}
}

static QList<int> parseWorkspaces(const QString &text, bool *ok)
{
	QList<int> out;
	*ok = true;
	for (const QString &tok : text.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts)) {
		bool n = false;
		int v = tok.toInt(&n);
		if (!n || v < 1 || v > 99)
			*ok = false;
		else if (!out.contains(v))
			out << v;
	}
	return out;
}

QList<QVariantMap> MainWindow::wallpaperTable() const
{
	QList<QVariantMap> list;
	for (int r = 0; r < wallpapers_->rowCount(); r++) {
		QVariantMap t;
		QTableWidgetItem *name = wallpapers_->item(r, WpName);
		t["name"] = name ? name->text().trimmed() : QString();
		t["paths"] = wallpapers_->item(r, WpPictures)->data(Qt::UserRole).toStringList();
		auto *spin = qobject_cast<QSpinBox *>(wallpapers_->cellWidget(r, WpInterval));
		if (spin && spin->value() != 600)
			t["interval"] = (qlonglong)spin->value();
		QWidget *cell = wallpapers_->cellWidget(r, WpAny);
		auto *radio = cell ? cell->findChild<QRadioButton *>() : nullptr;
		if (radio && radio->isChecked())
			t["any"] = true;
		bool ok;
		QVariantList ws;
		QTableWidgetItem *wi = wallpapers_->item(r, WpWorkspaces);
		for (int n : parseWorkspaces(wi ? wi->text() : QString(), &ok))
			ws << (qlonglong)n;
		if (!ws.isEmpty())
			t["workspaces"] = ws;
		list << t;
	}
	return list;
}

/* What is wrong with the workspaces column, if anything: not numbers, out of range, or given to two groups. */
QString MainWindow::wallpaperProblem() const
{
	QMap<int, QString> owner;
	for (int r = 0; r < wallpapers_->rowCount(); r++) {
		QTableWidgetItem *name = wallpapers_->item(r, WpName);
		QString who = name && !name->text().trimmed().isEmpty() ? name->text().trimmed() : tr("group %1").arg(r + 1);
		QTableWidgetItem *wi = wallpapers_->item(r, WpWorkspaces);
		bool ok;
		QList<int> ws = parseWorkspaces(wi ? wi->text() : QString(), &ok);
		if (!ok)
			return tr("The workspaces of “%1” must be numbers from 1 to 99, like 2, 5.").arg(who);
		for (int n : ws) {
			if (owner.contains(n))
				return tr("Workspace %1 is given to two groups (“%2” and “%3”); a workspace shows one group.")
					.arg(n).arg(owner[n], who);
			owner[n] = who;
		}
	}
	return QString();
}

static QTableWidget *makeTable(const QStringList &headers)
{
	auto *t = new QTableWidget(0, headers.size());
	t->setHorizontalHeaderLabels(headers);
	t->horizontalHeader()->setStretchLastSection(true);
	t->setSelectionBehavior(QAbstractItemView::SelectRows);
	return t;
}

/* The cursor themes installed here: asked of rei-wm, which knows where it looks (the icon directories and
 * /usr/share/cursors); without it, the same directories are scanned here. */
QStringList MainWindow::installedCursorThemes() const
{
	QStringList out;
	QString bin = findCompositor();
	if (!bin.isEmpty()) {
		QProcess p;
		p.start(bin, { "--list-cursor-themes" });
		if (p.waitForFinished(5000) && p.exitCode() == 0)
			for (const QString &l : QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts))
				out << l.trimmed();
		if (!out.isEmpty())
			return out;
	}
	QStringList dirs = qEnvironmentVariable("XCURSOR_PATH").split(':', Qt::SkipEmptyParts);
	if (dirs.isEmpty())
		dirs = { QDir::homePath() + "/.local/share/icons", QDir::homePath() + "/.icons", "/usr/share/icons",
			 "/usr/share/pixmaps", "/usr/share/cursors", "/usr/share/cursors/xorg-x11" };
	for (const QString &d : dirs)
		for (const QString &name : QDir(d).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
			if (QDir(d + "/" + name + "/cursors").exists() && !out.contains(name))
				out << name;
	out.sort();
	return out;
}

void MainWindow::updateCursorPreview()
{
	if (!cursorPreview_)
		return;
	QString theme, bin = findCompositor();
	int size = 24;
	for (const Field &f : std::as_const(fields_)) {
		if (f.section == "cursor" && f.key == "theme")
			theme = fieldValue(f).toString();
		if (f.section == "cursor" && f.key == "size")
			size = fieldValue(f).toInt();
	}
	if (bin.isEmpty()) {
		cursorPreview_->setText(tr("rei-wm was not found, so there is no preview."));
		return;
	}
	QTemporaryFile png(QDir::tempPath() + "/rei-cursor-XXXXXX.png");
	if (!png.open())
		return;
	png.close();
	QProcess p;
	p.start(bin, { "--cursor-preview", png.fileName(), "--cursor-theme", theme, "--cursor-size", QString::number(size) });
	if (!p.waitForFinished(8000) || p.exitCode() != 0) {
		cursorPreview_->setText(tr("The preview could not be drawn."));
		return;
	}
	cursorPreview_->setPixmap(QPixmap(png.fileName()));
}

QWidget *MainWindow::buildCursorTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *hint = new QLabel(tr("The pointer's look: the theme (an XCursor theme, from the icon directories or /usr/share/cursors) "
				   "and its size. rei-wm uses it at once for everything it draws and for programs that ask for a "
				   "named shape, and tells the programs and X11 apps it starts about it; programs that are already "
				   "running may keep their old pointer until they are restarted."));
	hint->setWordWrap(true);
	v->addWidget(hint);
	auto *f = new QFormLayout;
	auto *theme = static_cast<QComboBox *>(addField(f, tr("Theme"), "cursor", "theme", Choice, "", { "" }, 0, 0, 1,
							tr("Empty: the system's default theme ($XCURSOR_THEME, then “default”).")));
	theme->addItems(installedCursorThemes());
	theme->setEditable(true);
	theme->lineEdit()->setPlaceholderText(tr("system default"));
	addField(f, tr("Size (px)"), "cursor", "size", Int, 24, {}, 8, 256, 1,
		 tr("Logical pixels; on a scaled screen the picture is chosen to stay sharp."));
	v->addLayout(f);
	cursorPreview_ = new QLabel;
	cursorPreview_->setObjectName("cursor.preview");
	cursorPreview_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	v->addWidget(cursorPreview_);
	v->addStretch(1);
	for (const Field &fl : std::as_const(fields_)) {
		if (fl.section != "cursor")
			continue;
		if (fl.key == "theme")
			connect(static_cast<QComboBox *>(fl.widget), &QComboBox::currentTextChanged, this, [this] { updateCursorPreview(); });
		else
			connect(static_cast<QSpinBox *>(fl.widget), &QSpinBox::valueChanged, this, [this] { updateCursorPreview(); });
	}
	return w;
}

QWidget *MainWindow::buildAudioTab()
{
	auto *w = new QWidget;
	auto *f = new QFormLayout(w);
	addField(f, tr("Volume widget and media keys (PipeWire)"), "audio", "enabled", Bool, true, {}, 0, 0, 1,
		 tr("The toolbar's volume text (click it for the slider) and the volume media keys. Needs a restart to change."));
	addField(f, tr("Start the sound system automatically"), "audio", "autostart", Bool, true, {}, 0, 0, 1,
		 tr("Starts pipewire, wireplumber and pipewire-pulse if they are installed and not already running, "
		    "and stops what it started when rei-wm exits."));
	addField(f, tr("Volume step (percent)"), "audio", "volume_step", Int, 5, {}, 1, 100, 1,
		 tr("Per key press or wheel notch."));
	addField(f, tr("Maximum volume (percent)"), "audio", "max_volume", Int, 100, {}, 10, 150, 1,
		 tr("Above 100 amplifies and may distort. The volume slider spans 0 to this."));
	return w;
}

QWidget *MainWindow::buildMessagesTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *hint = new QLabel(tr("An invisible column of text at the edge of the screen. For now it shows what a command typed into "
				   "the Run prompt prints. A new message fades out after its time; each newer one pushes the "
				   "older ones up and dims them a step. Mod+n (the “messages” action) shows the whole history."));
	hint->setWordWrap(true);
	v->addWidget(hint);
	auto *f = new QFormLayout;
	addField(f, tr("Show messages"), "messages", "enabled", Bool, true);
	addField(f, tr("Side of the screen"), "messages", "position", Choice, "right", { "right", "left" });
	addField(f, tr("Time on screen (seconds)"), "messages", "duration", Double, 10.0, {}, 1, 600, 1,
		 tr("It fades during the last second and a half."));
	addField(f, tr("Messages shown at once"), "messages", "max_visible", Int, 6, {}, 1, 20);
	addField(f, tr("Column width (px)"), "messages", "width", Int, 480, {}, 120, 2000);
	addField(f, tr("Messages kept for the history"), "messages", "history", Int, 200, {}, 1, 5000);
	addField(f, tr("Font"), "messages", "font", Font, "Sans 12");
	addField(f, tr("Blur behind the view (px, 0 = none)"), "messages", "blur_radius", Int, 12, {}, 0, 64, 1,
		 tr("Mod+n is a top layer: the screen under it is blurred and darkened, and nothing reaches the windows."));
	addField(f, tr("Darken behind the view (0 to 0.9)"), "messages", "blur_dim", Double, 0.3, {}, 0, 0.9, 0.05);
	v->addLayout(f);
	v->addStretch(1);
	return w;
}

QWidget *MainWindow::buildMusicTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *hint = new QLabel(tr("Controls for MPD, the music player daemon: play, pause and skip, and the song that is playing, "
				   "at the bottom centre of the message view (Mod+n). With MPD not running there is a button that "
				   "starts it. When the song changes, “Now playing: …” is added to the messages."));
	hint->setWordWrap(true);
	v->addWidget(hint);
	auto *f = new QFormLayout;
	addField(f, tr("Music controls"), "mpd", "enabled", Bool, true);
	addField(f, tr("Announce a new song in the messages"), "mpd", "announce", Bool, true);
	addField(f, tr("Show the album cover"), "mpd", "album_art", Bool, true, {}, 0, 0, 1,
		 tr("The cover of the song playing (embedded in the file, or the cover.jpg/png next to it) in a square above the "
		    "music controls. Needs MPD 0.21 or newer."));
	addField(f, tr("MPD host"), "mpd", "host", String, "", {}, 0, 0, 1,
		 tr("Empty: $MPD_HOST, else the socket in $XDG_RUNTIME_DIR/mpd, else localhost. An IP address, localhost "
		    "or the path of a socket; “password@host” gives a password."));
	addField(f, tr("MPD port"), "mpd", "port", Int, 6600, {}, 1, 65535);
	addField(f, tr("Command that starts MPD"), "mpd", "start_command", String, "mpd", {}, 0, 0, 1,
		 tr("Run by the “Start MPD” button, e.g. “mpd” or “mpd ~/.config/mpd/mpd.conf”."));
	v->addLayout(f);
	v->addStretch(1);
	return w;
}

QWidget *MainWindow::buildThemeTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *f = new QFormLayout;
	/* The three colours every element's built-in look is derived from. Changing one moves every value
	 * below that you have not set by hand. */
	QWidget *base[3] = {
		addField(f, tr("Base background"), "toolbar", "background", Color, "#161c26", {}, 0, 0, 1,
			 tr("The toolbar's background; menus, tiles and boxes are derived from it.")),
		addField(f, tr("Base text"), "toolbar", "foreground", Color, "#d6dde8"),
		addField(f, tr("Base highlight"), "toolbar", "highlight", Color, "#3c6ebe", {}, 0, 0, 1,
			 tr("The accent: the focused workspace, active tiles and hovered rows.")),
	};
	addField(f, tr("Flavor icons folder"), "theme", "flavor_icons", Dir, "", {}, 0, 0, 1,
		 tr("A folder of square PNGs, at most 64x64. When set, every iconable element (tray tiles, the Start "
		    "button, Start menu rows) gets a random icon from it. Empty: off."));
	v->addLayout(f);
	theme_ = new ThemeEditor(findCompositor(), [this] { return buildDoc().text(); });
	theme_->setObjectName("theme_editor");
	connect(theme_, &ThemeEditor::edited, this, &MainWindow::markDirty);
	connect(theme_, &ThemeEditor::compositorChosen, this, [this](const QString &path) {
		QSettings("rei-wm", "settings").setValue("compositor", path);   /* remembered for next time */
		binary_ = path;
		theme_->setBinary(path);
		QString e;
		if (theme_->load(doc_.text(), &e))
			status_->setText(tr("Using %1 for the theme editor.").arg(path));
	});
	for (QWidget *b : base) {   /* the built-in look follows these */
		auto *cb = static_cast<ColorButton *>(b);
		cb->onChange = [this] {
			markDirty();
			if (!loading_)
				theme_->baseColorsChanged();
		};
	}
	v->addWidget(theme_, 1);
	return w;
}

QWidget *MainWindow::buildStartMenuTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *f = new QFormLayout;
	addField(f, tr("Show the Start button"), "start_menu", "enabled", Bool, true);
	addField(f, tr("Button label"), "start_menu", "label", String, "Start");
	addField(f, tr("Ask before logout, shutdown and reboot"), "start_menu", "confirm_power", Bool, true);
	addField(f, tr("Shutdown command"), "start_menu", "shutdown_command", String,
		 "loginctl poweroff || systemctl poweroff || poweroff");
	addField(f, tr("Reboot command"), "start_menu", "reboot_command", String,
		 "loginctl reboot || systemctl reboot || reboot");
	addField(f, tr("Logout command"), "start_menu", "logout_command", String, "", {}, 0, 0, 1,
		 tr("Empty: logging out just ends rei-wm."));
	addField(f, tr("Run prompt font"), "start_menu", "run_font", Font, "Sans 26", {}, 0, 0, 1,
		 tr("A Pango font description for the large Run prompt (Mod+r)."));
	v->addLayout(f);

	auto *hint = new QLabel(tr("<b>Shortcuts</b> appear at the top of the Start menu, in this order (at most 20). "
				   "Each runs its command through the shell when clicked. Double-click a cell to edit it."));
	hint->setWordWrap(true);
	v->addWidget(hint);
	shortcuts_ = makeTable({ tr("Name"), tr("Command") });
	shortcuts_->setObjectName("shortcuts");
	shortcuts_->setColumnWidth(0, 180);
	v->addWidget(shortcuts_, 1);
	auto *row = new QHBoxLayout;
	auto *add = new QPushButton(tr("Add"));
	auto *del = new QPushButton(tr("Remove"));
	auto *up = new QPushButton(tr("Move up"));
	auto *down = new QPushButton(tr("Move down"));
	add->setObjectName("shortcut_add");
	del->setObjectName("shortcut_remove");
	up->setObjectName("shortcut_up");
	down->setObjectName("shortcut_down");
	row->addStretch(1);
	row->addWidget(up);
	row->addWidget(down);
	row->addWidget(add);
	row->addWidget(del);
	v->addLayout(row);
	connect(add, &QPushButton::clicked, this, [this] {
		int r = shortcuts_->rowCount();
		shortcuts_->insertRow(r);
		shortcuts_->setItem(r, 0, new QTableWidgetItem(""));
		shortcuts_->setItem(r, 1, new QTableWidgetItem(""));
		shortcuts_->setCurrentCell(r, 0);
		shortcuts_->editItem(shortcuts_->item(r, 0));   /* start typing the name */
		markDirty();
	});
	connect(del, &QPushButton::clicked, this, [this] {
		auto rows = shortcuts_->selectionModel()->selectedRows();
		for (int i = rows.size() - 1; i >= 0; i--)
			shortcuts_->removeRow(rows[i].row());
		markDirty();
	});
	auto move = [this](int delta) {
		int r = shortcuts_->currentRow(), to = r + delta;
		if (r < 0 || to < 0 || to >= shortcuts_->rowCount())
			return;
		for (int c = 0; c < shortcuts_->columnCount(); c++) {
			QString a = shortcuts_->item(r, c) ? shortcuts_->item(r, c)->text() : QString();
			QString b = shortcuts_->item(to, c) ? shortcuts_->item(to, c)->text() : QString();
			shortcuts_->setItem(r, c, new QTableWidgetItem(b));
			shortcuts_->setItem(to, c, new QTableWidgetItem(a));
		}
		shortcuts_->setCurrentCell(to, shortcuts_->currentColumn() < 0 ? 0 : shortcuts_->currentColumn());
		markDirty();
	};
	connect(up, &QPushButton::clicked, this, [move] { move(-1); });
	connect(down, &QPushButton::clicked, this, [move] { move(1); });
	connect(shortcuts_, &QTableWidget::itemChanged, this, &MainWindow::markDirty);
	return w;
}

QWidget *MainWindow::buildKeysTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *hint = new QLabel(tr("“Mod” is the composition key. Actions: exec &lt;command&gt;, close, quit, focus_next, "
				   "focus_prev, focus|move left|right|up|down, split h|v, toggle_floating, fullscreen, minimize, "
				   "run, messages, workspace &lt;n&gt;, move_to_workspace &lt;n&gt;, none (removes a default binding)."));
	hint->setWordWrap(true);
	v->addWidget(hint);
	keys_ = makeTable({ tr("Keys"), tr("Action") });
	keys_->setObjectName("keys");
	keys_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	v->addWidget(keys_, 1);
	auto *row = new QHBoxLayout;
	auto *add = new QPushButton(tr("Add"));
	auto *del = new QPushButton(tr("Remove"));
	row->addStretch(1);
	row->addWidget(add);
	row->addWidget(del);
	v->addLayout(row);
	connect(add, &QPushButton::clicked, this, [this] {
		keys_->insertRow(keys_->rowCount());
		keys_->setItem(keys_->rowCount() - 1, 0, new QTableWidgetItem("Mod+"));
		keys_->setItem(keys_->rowCount() - 1, 1, new QTableWidgetItem(""));
		keys_->editItem(keys_->item(keys_->rowCount() - 1, 0));
		markDirty();
	});
	connect(del, &QPushButton::clicked, this, [this] {
		auto rows = keys_->selectionModel()->selectedRows();
		for (int i = rows.size() - 1; i >= 0; i--)
			keys_->removeRow(rows[i].row());
		markDirty();
	});
	connect(keys_, &QTableWidget::itemChanged, this, &MainWindow::markDirty);
	return w;
}

QWidget *MainWindow::buildOutputsTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *hint = new QLabel(tr("With the DRM backend the name is the connector shown by <tt>rei-wm --drm-info</tt> "
				   "(DP-1, HDMI-A-1, eDP-1…). Mode is like 2560x1440@144; leave it empty for the monitor's "
				   "preferred mode. X and Y are optional, and both must be given. Changes here need a restart "
				   "of rei-wm."));
	hint->setWordWrap(true);
	v->addWidget(hint);
	outputs_ = makeTable({ tr("Name"), tr("Mode"), tr("Scale"), tr("X"), tr("Y"), tr("Enabled") });
	outputs_->setObjectName("outputs");
	v->addWidget(outputs_, 1);
	auto *row = new QHBoxLayout;
	auto *add = new QPushButton(tr("Add"));
	auto *del = new QPushButton(tr("Remove"));
	row->addStretch(1);
	row->addWidget(add);
	row->addWidget(del);
	v->addLayout(row);
	connect(add, &QPushButton::clicked, this, [this] {
		int r = outputs_->rowCount();
		outputs_->insertRow(r);
		outputs_->setItem(r, 0, new QTableWidgetItem(""));
		outputs_->setItem(r, 1, new QTableWidgetItem(""));
		outputs_->setItem(r, 2, new QTableWidgetItem("1.0"));
		outputs_->setItem(r, 3, new QTableWidgetItem(""));
		outputs_->setItem(r, 4, new QTableWidgetItem(""));
		auto *enabled = new QTableWidgetItem;
		enabled->setCheckState(Qt::Checked);
		outputs_->setItem(r, 5, enabled);
		markDirty();
	});
	connect(del, &QPushButton::clicked, this, [this] {
		auto rows = outputs_->selectionModel()->selectedRows();
		for (int i = rows.size() - 1; i >= 0; i--)
			outputs_->removeRow(rows[i].row());
		markDirty();
	});
	connect(outputs_, &QTableWidget::itemChanged, this, &MainWindow::markDirty);
	return w;
}

/* ------------------------------------------------------------------------ */
/* Loading                                                                  */
/* ------------------------------------------------------------------------ */

void MainWindow::loadShortcuts()
{
	shortcuts_->setRowCount(0);
	for (const QVariantMap &t : doc_.tables("shortcut")) {
		int r = shortcuts_->rowCount();
		shortcuts_->insertRow(r);
		shortcuts_->setItem(r, 0, new QTableWidgetItem(t.value("name").toString()));
		shortcuts_->setItem(r, 1, new QTableWidgetItem(t.value("command").toString()));
	}
}

QList<QVariantMap> MainWindow::shortcutTable() const
{
	QList<QVariantMap> list;
	auto text = [this](int r, int c) {
		return shortcuts_->item(r, c) ? shortcuts_->item(r, c)->text().trimmed() : QString();
	};
	for (int r = 0; r < shortcuts_->rowCount(); r++) {
		QString name = text(r, 0), command = text(r, 1);
		if (name.isEmpty() && command.isEmpty())
			continue;   /* a blank row is just an unfinished one */
		QVariantMap t;
		t["name"] = name;   /* (a half-filled row is kept, so rei-wm's check can say what is missing) */
		t["command"] = command;
		list << t;
	}
	return list;
}

void MainWindow::loadKeys()
{
	keys_->setRowCount(0);
	loadedKeys_.clear();
	for (const QString &k : doc_.keys("keybindings")) {
		int r = keys_->rowCount();
		keys_->insertRow(r);
		QString action = doc_.get("keybindings", k).toString();
		keys_->setItem(r, 0, new QTableWidgetItem(k));
		keys_->setItem(r, 1, new QTableWidgetItem(action));
		loadedKeys_[k] = action;
	}
}

void MainWindow::loadOutputs()
{
	outputs_->setRowCount(0);
	for (const QVariantMap &t : doc_.tables("output")) {
		int r = outputs_->rowCount();
		outputs_->insertRow(r);
		QString mode = t.value("mode").toString();
		if (mode.isEmpty() && t.contains("width") && t.contains("height")) {
			mode = QString("%1x%2").arg(t.value("width").toInt()).arg(t.value("height").toInt());
			if (t.contains("refresh"))
				mode += "@" + QString::number(t.value("refresh").toDouble(), 'g', 6);
		}
		outputs_->setItem(r, 0, new QTableWidgetItem(t.value("name").toString()));
		outputs_->setItem(r, 1, new QTableWidgetItem(mode));
		outputs_->setItem(r, 2, new QTableWidgetItem(t.contains("scale") ? QString::number(t.value("scale").toDouble(), 'g', 6) : "1.0"));
		outputs_->setItem(r, 3, new QTableWidgetItem(t.contains("x") ? QString::number(t.value("x").toInt()) : ""));
		outputs_->setItem(r, 4, new QTableWidgetItem(t.contains("y") ? QString::number(t.value("y").toInt()) : ""));
		auto *enabled = new QTableWidgetItem;
		enabled->setCheckState(t.value("enabled", true).toBool() ? Qt::Checked : Qt::Unchecked);
		outputs_->setItem(r, 5, enabled);
	}
}

QString MainWindow::findCompositor() const
{
	if (!binary_.isEmpty())
		return binary_;
	QString env = qEnvironmentVariable("REI_WM_BIN");
	if (!env.isEmpty())
		return env;
	QString chosen = QSettings("rei-wm", "settings").value("compositor").toString();   /* "Locate rei-wm…" */
	if (!chosen.isEmpty() && QFile::exists(chosen))
		return chosen;
	QString sibling = QCoreApplication::applicationDirPath() + "/rei-wm";
	if (QFile::exists(sibling))
		return sibling;
	/* In a meson build tree the settings app is in settings/ and the compositor in src/. */
	QString built = QCoreApplication::applicationDirPath() + "/../src/rei-wm";
	if (QFile::exists(built))
		return QFileInfo(built).canonicalFilePath();
	return QStandardPaths::findExecutable("rei-wm");
}

bool MainWindow::load(QString *error)
{
	QFile f(path_);
	QString text;
	if (f.open(QIODevice::ReadOnly)) {
		text = QString::fromUtf8(f.readAll());
	} else {
		/* No config yet: start from the compositor's own default file. */
		QString bin = findCompositor();
		if (!bin.isEmpty()) {
			QProcess p;
			p.start(bin, { "--print-default-config" });
			if (p.waitForFinished(5000) && p.exitCode() == 0)
				text = QString::fromUtf8(p.readAllStandardOutput());
		}
		if (text.isEmpty() && error)
			*error = tr("Cannot read %1 (and found no rei-wm to create a default one); changes will create it.")
					 .arg(path_);
	}
	doc_ = TomlDoc::parse(text);

	loading_ = true;
	for (Field &f2 : fields_) {
		QVariant v = doc_.get(f2.section, f2.key);
		if (!v.isValid())
			v = f2.fallback;
		setFieldValue(f2, v);
	}
	loadKeys();
	{
		QString themeErr;
		if (!theme_->load(doc_.text(), &themeErr))
			status_->setText(themeErr);
	}
	loadShortcuts();
	loadedShortcuts_ = shortcutTable();
	loadOutputs();
	loadedOutputs_ = outputTable();  /* In the table's own normal form, to compare against. */
	loadWallpapers();
	loadedWallpapers_ = wallpaperTable();
	loading_ = false;
	dirty_ = false;
	status_->setText(QFile::exists(path_) ? tr("Loaded %1").arg(path_)
					      : tr("%1 does not exist yet; saving will create it.").arg(path_));
	return true;
}

/* ------------------------------------------------------------------------ */
/* Saving                                                                   */
/* ------------------------------------------------------------------------ */

bool MainWindow::dirty() const { return dirty_; }

void MainWindow::markDirty()
{
	if (loading_)
		return;
	dirty_ = true;
	status_->setText(tr("Unsaved changes."));
}

QMap<QString, QString> MainWindow::keyTable() const
{
	QMap<QString, QString> m;
	for (int r = 0; r < keys_->rowCount(); r++) {
		QString k = keys_->item(r, 0) ? keys_->item(r, 0)->text().trimmed() : QString();
		QString a = keys_->item(r, 1) ? keys_->item(r, 1)->text().trimmed() : QString();
		if (!k.isEmpty() && !a.isEmpty())
			m[k] = a;
	}
	return m;
}

QList<QVariantMap> MainWindow::outputTable() const
{
	QList<QVariantMap> list;
	auto text = [this](int r, int c) {
		return outputs_->item(r, c) ? outputs_->item(r, c)->text().trimmed() : QString();
	};
	for (int r = 0; r < outputs_->rowCount(); r++) {
		QVariantMap t;
		if (!text(r, 0).isEmpty())
			t["name"] = text(r, 0);
		if (!text(r, 1).isEmpty())
			t["mode"] = text(r, 1);
		bool ok = false;
		double scale = text(r, 2).toDouble(&ok);
		if (ok && scale != 1.0)
			t["scale"] = scale;
		if (!text(r, 3).isEmpty() && !text(r, 4).isEmpty()) {
			t["x"] = (qlonglong)text(r, 3).toInt();
			t["y"] = (qlonglong)text(r, 4).toInt();
		} else if (!text(r, 3).isEmpty() || !text(r, 4).isEmpty()) {
			t["x"] = (qlonglong)text(r, 3).toInt();  /* The check will say both are needed. */
		}
		if (outputs_->item(r, 5) && outputs_->item(r, 5)->checkState() != Qt::Checked)
			t["enabled"] = false;
		list << t;
	}
	return list;
}

bool MainWindow::validate(const QString &text, QString *error)
{
	QString bin = findCompositor();
	if (bin.isEmpty()) {
		status_->setText(tr("rei-wm was not found, so the file could not be checked before saving."));
		return true;
	}
	QTemporaryFile tmp(QDir::tempPath() + "/rei-settings-XXXXXX.toml");
	if (!tmp.open() || tmp.write(text.toUtf8()) < 0) {
		if (error)
			*error = tr("Cannot write a temporary file to check the settings.");
		return false;
	}
	tmp.flush();
	QProcess p;
	p.start(bin, { "--check-config", "--config", tmp.fileName() });
	if (!p.waitForFinished(10000)) {
		if (error)
			*error = tr("Checking the settings timed out.");
		return false;
	}
	if (p.exitCode() != 0) {
		if (error) {
			QString msg = QString::fromUtf8(p.readAllStandardError()).trimmed();
			msg.replace(tmp.fileName(), tr("the new settings"));
			*error = msg.isEmpty() ? tr("rei-wm rejected the settings.") : msg;
		}
		return false;
	}
	return true;
}

TomlDoc MainWindow::buildDoc() const
{
	TomlDoc doc = doc_;

	for (const Field &f : fields_) {
		QVariant now = fieldValue(f);
		if (now == f.loaded)
			continue;  /* Untouched: leave the file's own line (or absence) alone. */
		/* Strings the user emptied stay as explicit empty strings, which is meaningful. */
		doc.set(f.section, f.key, now);
	}

	QMap<QString, QString> keys = keyTable();
	if (keys != loadedKeys_) {
		for (auto it = loadedKeys_.begin(); it != loadedKeys_.end(); ++it)
			if (!keys.contains(it.key()))
				doc.remove("keybindings", it.key());
		for (auto it = keys.begin(); it != keys.end(); ++it)
			if (loadedKeys_.value(it.key()) != it.value())
				doc.set("keybindings", it.key(), it.value());
	}

	QList<QVariantMap> shorts = shortcutTable();
	if (shorts != loadedShortcuts_)
		doc.setTables("shortcut", shorts);

	QList<QVariantMap> outs = outputTable();
	if (outs != loadedOutputs_) {
		/* Rewritten as a block; compare against what the table would have produced from the
		 * loaded values so that merely opening the tab does not rewrite the file. */
		doc.setTables("output", outs);
	}

	QList<QVariantMap> groups = wallpaperTable();
	if (groups != loadedWallpapers_) {
		doc.setTables("wallpaper_group", groups);
		doc.remove("wallpaper", "path");   /* the old single picture is one of the groups now */
	}

	if (theme_)
		theme_->apply(doc);
	return doc;
}

bool MainWindow::save(QString *error)
{
	QString problem = wallpaperProblem();
	if (!problem.isEmpty()) {
		if (error)
			*error = problem;
		status_->setText(tr("Not saved: %1").arg(problem));
		return false;
	}
	TomlDoc doc = buildDoc();
	QList<QVariantMap> shorts = shortcutTable();
	QList<QVariantMap> outs = outputTable();

	QString text = doc.text();
	QString why;
	if (!validate(text, &why)) {
		if (error)
			*error = why;
		status_->setText(tr("Not saved: %1").arg(why));
		return false;
	}

	QDir().mkpath(QFileInfo(path_).absolutePath());
	QSaveFile out(path_);  /* Written to a temporary file and renamed: the compositor never sees half a file. */
	if (!out.open(QIODevice::WriteOnly) || out.write(text.toUtf8()) < 0 || !out.commit()) {
		QString msg = tr("Cannot write %1: %2").arg(path_, out.errorString());
		if (error)
			*error = msg;
		status_->setText(msg);
		return false;
	}

	doc_ = TomlDoc::parse(text);
	QString ignore;
	/* Re-sync "loaded" values with what was just saved. */
	loading_ = true;
	for (Field &f : fields_)
		f.loaded = fieldValue(f);
	loadedKeys_ = keyTable();
	loadedOutputs_ = outs;
	loadedShortcuts_ = shorts;
	loadedWallpapers_ = wallpaperTable();
	theme_->markSaved();
	loading_ = false;
	dirty_ = false;
	status_->setText(tr("Saved. A running rei-wm applies most changes immediately; "
			    "monitor and backend changes need a restart."));
	return true;
}

void MainWindow::onSaveClicked()
{
	QString err;
	if (!save(&err))
		QMessageBox::warning(this, tr("Settings not saved"), err);
}

void MainWindow::onRevertClicked()
{
	QString err;
	load(&err);
}
