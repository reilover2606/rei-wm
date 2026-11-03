#include "mainwindow.h"

#include <QApplication>
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
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QVBoxLayout>
#include <functional>

namespace {

/* A colour swatch button that shows and edits "#rrggbb". */
class ColorButton : public QPushButton {
public:
	explicit ColorButton(QWidget *parent = nullptr) : QPushButton(parent)
	{
		connect(this, &QPushButton::clicked, this, [this] {
			QColor c = QColorDialog::getColor(color_, this, "Choose a colour");
			if (c.isValid()) {
				setColor(c.name());
				if (onChange)
					onChange();
			}
		});
	}
	std::function<void()> onChange;
	QString color() const { return color_.name(); }
	void setColor(const QString &name)
	{
		color_ = QColor(name);
		if (!color_.isValid())
			color_ = QColor("#000000");
		setText(color_.name());
		setStyleSheet(QString("background-color: %1; color: %2;")
				      .arg(color_.name(), color_.lightness() > 128 ? "black" : "white"));
	}
private:
	QColor color_ = QColor("#000000");
};

}  // namespace

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
	tabs->addTab(buildLookTab(), tr("Look"));
	tabs->addTab(buildKeysTab(), tr("Keybindings"));
	tabs->addTab(buildOutputsTab(), tr("Monitors"));
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
		c->setCurrentIndex(i >= 0 ? i : 0);
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
	addField(f, tr("Focus follows the mouse"), "general", "focus_follows_mouse", Bool, false);
	addField(f, tr("Run Xwayland (X11 programs)"), "general", "xwayland", Bool, true,
		 {}, 0, 0, 1, tr("Needs a restart of rei-wm to change."));
	addField(f, tr("Volume control (PipeWire)"), "audio", "enabled", Bool, true, {}, 0, 0, 1,
		 tr("The toolbar's volume text and the media keys. Needs a restart to change."));
	addField(f, tr("Volume step (percent)"), "audio", "volume_step", Int, 5, {}, 1, 100);
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
	addField(f, tr("Font"), "toolbar", "font", Font, "Sans 11", {}, 0, 0, 1,
		 tr("A Pango font description, e.g. “Sans 11”."));
	addField(f, tr("Clock format"), "toolbar", "clock_format", String, "%H:%M:%S", {}, 0, 0, 1,
		 tr("strftime format; empty hides the clock."));
	addField(f, tr("Background"), "toolbar", "background", Color, "#161c26");
	addField(f, tr("Text"), "toolbar", "foreground", Color, "#d6dde8");
	addField(f, tr("Highlight"), "toolbar", "highlight", Color, "#3c6ebe");
	return w;
}

QWidget *MainWindow::buildLookTab()
{
	auto *w = new QWidget;
	auto *f = new QFormLayout(w);
	addField(f, tr("Wallpaper image"), "wallpaper", "path", Path, "");
	addField(f, tr("Wallpaper fit"), "wallpaper", "mode", Choice, "fill",
		 { "fill", "fit", "stretch", "center", "tile" });
	addField(f, tr("Background colour"), "wallpaper", "color", Color, "#1b2733");
	return w;
}

static QTableWidget *makeTable(const QStringList &headers)
{
	auto *t = new QTableWidget(0, headers.size());
	t->setHorizontalHeaderLabels(headers);
	t->horizontalHeader()->setStretchLastSection(true);
	t->setSelectionBehavior(QAbstractItemView::SelectRows);
	return t;
}

QWidget *MainWindow::buildKeysTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *hint = new QLabel(tr("“Mod” is the composition key. Actions: exec &lt;command&gt;, close, quit, focus_next, "
				   "focus_prev, focus|move left|right|up|down, split h|v, toggle_floating, fullscreen, minimize, "
				   "workspace &lt;n&gt;, move_to_workspace &lt;n&gt;, none (removes a default binding)."));
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
	QString sibling = QCoreApplication::applicationDirPath() + "/rei-wm";
	if (QFile::exists(sibling))
		return sibling;
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
	loadOutputs();
	loadedOutputs_ = outputTable();  /* In the table's own normal form, to compare against. */
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

bool MainWindow::save(QString *error)
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

	QList<QVariantMap> outs = outputTable();
	if (outs != loadedOutputs_) {
		/* Rewritten as a block; compare against what the table would have produced from the
		 * loaded values so that merely opening the tab does not rewrite the file. */
		doc.setTables("output", outs);
	}

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
