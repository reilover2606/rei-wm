#include "themeeditor.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QToolButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>

#include "colorbutton.h"

namespace {

struct ElementInfo {
	const char *key;
	const char *title;
	const char *what;
};

/* In the order rei-wm lists them. */
const ElementInfo kElements[] = {
	{ "toolbar", "Toolbar", "The bar's own background and its text." },
	{ "workspace_button", "Workspace buttons", "A workspace number: normal, the focused one (active), one showing on another monitor (inactive)." },
	{ "tray_tile", "Tray tiles", "An application in the tray. Active: it has the focus; inactive: minimized." },
	{ "tray_overflow", "Tray overflow tile", "The +N tile that holds windows that no longer fit." },
	{ "tooltip", "Tooltip", "The text that appears when you hover a tray tile." },
	{ "menu", "Menus", "The box of a pop-up list (the Start menu, a tray tile's window list)." },
	{ "menu_row", "Menu rows", "A row of a menu: active colours and the glow are used while hovered." },
	{ "start_button", "Start button", "The Start button (active while its menu is open); also the buttons of dialogs." },
	{ "run_prompt", "Run prompt", "The large, see-through command box (Mod+r)." },
	{ "messages", "Message column", "Text colour, the dark halo (shadow) around the text, and the stack view's backdrop (colour, opacity, shape)." },
	{ "sysinfo", "CPU/RAM readout", "The box left of the clock." },
	{ "dialog", "Dialogs", "Confirmation dialogs (the box; their buttons use the Start button's look)." },
	{ "battery", "Battery", "The battery icon: colour = level, active = charging, inactive = low." },
	{ "window_text", "Window text (terminals)", "Not rei-wm's own text: the windows of the listed programs are drawn through a "
	  "fragment shader that makes their bright pixels (a terminal's text) glow. Size 0 turns it off." },
};

struct PropInfo {
	const char *name;
	const char *label;
	const char *kind;   /* color, number, shape, bool */
	double lo, hi, step;
	int decimals;
};

const PropInfo kColors[] = {
	{ "color", "Colour", "color", 0, 0, 0, 0 },
	{ "active_color", "Active colour", "color", 0, 0, 0, 0 },
	{ "inactive_color", "Inactive colour", "color", 0, 0, 0, 0 },
};
const PropInfo kText[] = {
	{ "text_color", "Text", "color", 0, 0, 0, 0 },
	{ "active_text_color", "Active text", "color", 0, 0, 0, 0 },
	{ "inactive_text_color", "Inactive text", "color", 0, 0, 0, 0 },
};
const PropInfo kShape[] = {
	{ "shape", "Shape", "shape", 0, 0, 0, 0 },
	{ "radius", "Corner radius (px)", "number", 0, 100, 1, 1 },
	{ "opacity", "Opacity of the fill", "number", 0, 1, 0.05, 2 },
	{ "border_color", "Border colour", "color", 0, 0, 0, 0 },
	{ "border_width", "Border width (px)", "number", 0, 20, 0.5, 1 },
};
const PropInfo kShadow[] = {
	{ "shadow_color", "Shadow colour", "color", 0, 0, 0, 0 },
	{ "shadow_opacity", "Shadow opacity (0 = none)", "number", 0, 1, 0.05, 2 },
	{ "shadow_offset_x", "Offset X (px)", "number", -50, 50, 1, 1 },
	{ "shadow_offset_y", "Offset Y (px)", "number", -50, 50, 1, 1 },
	{ "shadow_blur", "Blur (px)", "number", 0, 50, 1, 1 },
};
const PropInfo kGlow[] = {
	{ "glow_color", "Glow colour", "color", 0, 0, 0, 0 },
	{ "glow_size", "Glow size (px, 0 = none)", "number", 0, 60, 1, 1 },
	{ "glow_opacity", "Glow opacity", "number", 0, 1, 0.05, 2 },
};

/* Effects on an element's text, drawn by a fragment shader. Each is off at 0. */
const PropInfo kTextFx[] = {
	{ "text_glow_size", "Glow size (px, 0 = none)", "number", 0, 40, 1, 1 },
	{ "text_glow_strength", "Glow strength", "number", 0, 4, 0.1, 2 },
	{ "text_glow_tint", "Glow tint (0 = the text's own colour)", "number", 0, 1, 0.05, 2 },
	{ "text_glow_color", "Glow tint colour", "color", 0, 0, 0, 0 },
	{ "text_blink", "Blink (seconds per cycle, 0 = steady)", "number", 0, 30, 0.1, 2 },
	{ "text_shine", "Metallic shine (0 = none)", "number", 0, 1, 0.05, 2 },
	{ "text_shine_color", "Shine colour", "color", 0, 0, 0, 0 },
};
/* [theme.window_text]: the window shader for terminals. */
const PropInfo kWindowText[] = {
	{ "apps", "Programs (app ids / X11 classes, comma separated; * = all)", "string", 0, 0, 0, 0 },
	{ "shader", "Shader (\"glow\", or the path of a GLSL file)", "string", 0, 0, 0, 0 },
	{ "text_glow_size", "Glow size (px, 0 = none)", "number", 0, 40, 1, 1 },
	{ "text_glow_strength", "Glow strength", "number", 0, 4, 0.1, 2 },
	{ "glow_threshold", "Only pixels brighter than (0..1)", "number", 0, 1, 0.05, 2 },
	{ "glow_area_limit", "No glow on mostly bright rows, above this share (0..1; 1 = always glow): keeps highlighted lines readable", "number", 0, 1, 0.05, 2 },
	{ "text_glow_tint", "Glow tint (0 = the text's own colour)", "number", 0, 1, 0.05, 2 },
	{ "text_glow_color", "Glow tint colour", "color", 0, 0, 0, 0 },
	{ "text_blink", "Blink (seconds per cycle, 0 = steady)", "number", 0, 30, 0.1, 2 },
};

QString normalized(const QString &kind, const QString &v)
{
	if (kind == "number") {
		bool ok = false;
		double d = v.toDouble(&ok);
		return ok ? QString::number(d, 'g', 6) : v;
	}
	if (kind == "color")
		return QColor(v).isValid() ? QColor(v).name() : v;
	return v;
}

}  // namespace

ThemeEditor::ThemeEditor(const QString &compositorBinary, std::function<QString()> textProvider, QWidget *parent)
	: QWidget(parent), binary_(compositorBinary), text_(std::move(textProvider))
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	/* Shown (instead of a silently grayed-out editor) when rei-wm cannot be asked for the theme. */
	noticeBox_ = new QWidget;
	noticeBox_->setObjectName("theme_notice_box");
	auto *noticeLayout = new QHBoxLayout(noticeBox_);
	noticeLayout->setContentsMargins(6, 6, 6, 6);
	notice_ = new QLabel;
	notice_->setObjectName("theme_notice");
	notice_->setWordWrap(true);
	notice_->setStyleSheet("color: #a02020;");
	noticeLayout->addWidget(notice_, 1);
	auto *locate = new QPushButton(tr("Locate rei-wm…"));
	locate->setObjectName("theme_locate");
	connect(locate, &QPushButton::clicked, this, [this] {
		QString f = QFileDialog::getOpenFileName(this, tr("Choose the rei-wm program"), QString());
		if (!f.isEmpty()) {
			binary_ = f;
			emit compositorChosen(f);
		}
	});
	noticeLayout->addWidget(locate);
	noticeBox_->hide();
	outer->addWidget(noticeBox_);

	auto *split = new QSplitter(Qt::Vertical);
	body_ = split;
	outer->addWidget(split, 1);

	auto *top = new QWidget;
	auto *topLayout = new QHBoxLayout(top);
	topLayout->setContentsMargins(0, 0, 0, 0);
	auto *left = new QWidget;
	auto *leftLayout = new QVBoxLayout(left);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	list_ = new QListWidget;
	list_->setObjectName("theme_elements");
	leftLayout->addWidget(list_, 1);
	auto *resetAllButton = new QPushButton(tr("Reset everything"));
	resetAllButton->setObjectName("theme_reset_all");
	resetAllButton->setToolTip(tr("Returns every element to the built-in look (for the toolbar colours above)."));
	connect(resetAllButton, &QPushButton::clicked, this, &ThemeEditor::resetAll);
	leftLayout->addWidget(resetAllButton);
	left->setMaximumWidth(190);
	topLayout->addWidget(left);
	pages_ = new QStackedWidget;
	topLayout->addWidget(pages_, 1);
	split->addWidget(top);

	auto *previewBox = new QWidget;
	auto *pv = new QVBoxLayout(previewBox);
	pv->setContentsMargins(0, 0, 0, 0);
	note_ = new QLabel(tr("Preview (drawn by rei-wm, so it matches what you will see)"));
	pv->addWidget(note_);
	auto *scroll = new QScrollArea;
	preview_ = new QLabel;
	preview_->setObjectName("theme_preview");
	preview_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	scroll->setWidget(preview_);
	scroll->setMinimumHeight(180);
	pv->addWidget(scroll, 1);
	split->addWidget(previewBox);
	split->setStretchFactor(0, 3);
	split->setStretchFactor(1, 2);

	for (const ElementInfo &e : kElements) {
		auto *item = new QListWidgetItem(tr(e.title));
		item->setData(Qt::UserRole, e.key);
		list_->addItem(item);
		bool iconable = QString(e.key) == "tray_tile" || QString(e.key) == "start_button" || QString(e.key) == "menu_row";
		buildPage(e.key, iconable);
	}
	connect(list_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
	list_->setCurrentRow(0);

	timer_ = new QTimer(this);
	timer_->setSingleShot(true);
	timer_->setInterval(350);
	connect(timer_, &QTimer::timeout, this, &ThemeEditor::startPreview);
}

ThemeEditor::~ThemeEditor()
{
	if (process_) {   /* a preview still being drawn: stop it before its widgets go */
		process_->disconnect();
		process_->kill();
		process_->waitForFinished(2000);
		delete process_;
		process_ = nullptr;
		QFile::remove(previewPath_);
		QFile::remove(previewCfg_);
	}
}

void ThemeEditor::buildPage(const QString &element, bool iconable)
{
	const ElementInfo *info = nullptr;
	for (const ElementInfo &e : kElements)
		if (element == e.key)
			info = &e;

	auto *page = new QWidget;
	auto *v = new QVBoxLayout(page);
	auto *what = new QLabel(tr(info->what));
	what->setWordWrap(true);
	v->addWidget(what);
	auto *resetElem = new QPushButton(tr("Reset this element to the built-in look"));
	resetElem->setObjectName("reset_element." + element);
	connect(resetElem, &QPushButton::clicked, this, [this, element] { resetElement(element); });
	v->addWidget(resetElem, 0, Qt::AlignLeft);

	auto group = [&](const QString &title, const PropInfo *list, size_t n) {
		auto *box = new QGroupBox(title);
		auto *form = new QFormLayout(box);
		for (size_t i = 0; i < n; i++) {
			const PropInfo &pi = list[i];
			Prop p;
			p.name = pi.name;
			p.kind = pi.kind;
			QWidget *w = nullptr;
			if (p.kind == "color") {
				auto *b = new ColorButton;
				const QString nm = pi.name;
				b->onChange = [this, element, nm] { touch(element, nm); };
				w = b;
			} else if (p.kind == "shape") {
				auto *c = new QComboBox;
				c->addItems({ "square", "rounded", "pill", "bevel" });
				const QString nm = pi.name;
				connect(c, &QComboBox::currentTextChanged, this, [this, element, nm] { touch(element, nm); });
				w = c;
			} else if (p.kind == "string") {
				auto *e = new QLineEdit;
				const QString nm = pi.name;
				connect(e, &QLineEdit::textChanged, this, [this, element, nm] { touch(element, nm); });
				w = e;
			} else {
				auto *s = new QDoubleSpinBox;
				s->setRange(pi.lo, pi.hi);
				s->setSingleStep(pi.step);
				s->setDecimals(pi.decimals);
				const QString nm = pi.name;
				connect(s, &QDoubleSpinBox::valueChanged, this, [this, element, nm] { touch(element, nm); });
				w = s;
			}
			const QString nm = pi.name;
			w->setObjectName("theme." + element + "." + pi.name);
			auto *rowWidget = new QWidget;
			auto *rowLayout = new QHBoxLayout(rowWidget);
			rowLayout->setContentsMargins(0, 0, 0, 0);
			rowLayout->addWidget(w, 1);
			auto *reset = new QToolButton;
			reset->setText(QStringLiteral("↺"));
			reset->setObjectName("reset." + element + "." + pi.name);
			reset->setAutoRaise(true);
			connect(reset, &QToolButton::clicked, this, [this, element, nm] {
				Prop &pr = props_[element][nm];
				setValue(pr, pr.defaultV);
				touch(element, nm);
			});
			rowLayout->addWidget(reset);
			form->addRow(tr(pi.label), rowWidget);
			p.widget = w;
			p.reset = reset;
			props_[element][p.name] = p;
		}
		v->addWidget(box);
	};
	if (element == "window_text") {
		group(tr("Window shader"), kWindowText, sizeof kWindowText / sizeof *kWindowText);
	} else {
		group(tr("Colours"), kColors, sizeof kColors / sizeof *kColors);
		group(tr("Text colours"), kText, sizeof kText / sizeof *kText);
		group(tr("Text effects (glow, blink, shine)"), kTextFx, sizeof kTextFx / sizeof *kTextFx);
		group(tr("Shape, fill and border"), kShape, sizeof kShape / sizeof *kShape);
		group(tr("Shadow"), kShadow, sizeof kShadow / sizeof *kShadow);
		group(tr("Glow"), kGlow, sizeof kGlow / sizeof *kGlow);
	}
	if (iconable) {
		auto *box = new QGroupBox(tr("Icon"));
		auto *form = new QFormLayout(box);
		Prop on;
		on.name = "icon";
		on.kind = "bool";
		auto *c = new QCheckBox;
		connect(c, &QCheckBox::toggled, this, [this, element] { touch(element, "icon"); });
		c->setObjectName("theme." + element + ".icon");
		on.widget = c;
		auto *iconReset = new QToolButton;
		iconReset->setText(QStringLiteral("↺"));
		iconReset->setAutoRaise(true);
		iconReset->setObjectName("reset." + element + ".icon");
		connect(iconReset, &QToolButton::clicked, this, [this, element] {
			setValue(props_[element]["icon"], props_[element]["icon"].defaultV);
			touch(element, "icon");
		});
		on.reset = iconReset;
		auto *iconRow = new QWidget;
		auto *iconRowLayout = new QHBoxLayout(iconRow);
		iconRowLayout->setContentsMargins(0, 0, 0, 0);
		iconRowLayout->addWidget(c, 1);
		iconRowLayout->addWidget(iconReset);
		form->addRow(tr("Show an icon"), iconRow);
		props_[element]["icon"] = on;
		Prop sz;
		sz.name = "icon_size";
		sz.kind = "number";
		auto *s = new QDoubleSpinBox;
		s->setRange(0, 64);
		s->setDecimals(0);
		connect(s, &QDoubleSpinBox::valueChanged, this, [this, element] { touch(element, "icon_size"); });
		s->setObjectName("theme." + element + ".icon_size");
		s->setToolTip(tr("Pixels, at most 64; 0 fits the element."));
		sz.widget = s;
		auto *sizeReset = new QToolButton;
		sizeReset->setText(QStringLiteral("↺"));
		sizeReset->setAutoRaise(true);
		sizeReset->setObjectName("reset." + element + ".icon_size");
		connect(sizeReset, &QToolButton::clicked, this, [this, element] {
			setValue(props_[element]["icon_size"], props_[element]["icon_size"].defaultV);
			touch(element, "icon_size");
		});
		sz.reset = sizeReset;
		auto *sizeRow = new QWidget;
		auto *sizeRowLayout = new QHBoxLayout(sizeRow);
		sizeRowLayout->setContentsMargins(0, 0, 0, 0);
		sizeRowLayout->addWidget(s, 1);
		sizeRowLayout->addWidget(sizeReset);
		form->addRow(tr("Icon size (px, 0 = fit)"), sizeRow);
		props_[element]["icon_size"] = sz;
		v->addWidget(box);
	}
	v->addStretch(1);

	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setWidget(page);
	pageOf_[element] = pages_->addWidget(scroll);
}

QString ThemeEditor::valueOf(const Prop &p) const
{
	QString raw;
	if (p.kind == "color")
		raw = static_cast<ColorButton *>(p.widget)->color();
	else if (p.kind == "shape")
		raw = static_cast<QComboBox *>(p.widget)->currentText();
	else if (p.kind == "bool")
		raw = static_cast<QCheckBox *>(p.widget)->isChecked() ? "true" : "false";
	else if (p.kind == "string")
		raw = static_cast<QLineEdit *>(p.widget)->text();
	else
		raw = QString::number(static_cast<QDoubleSpinBox *>(p.widget)->value(), 'g', 6);
	return normalized(p.kind, raw);
}

void ThemeEditor::setValue(Prop &p, const QString &text)
{
	if (p.kind == "color")
		static_cast<ColorButton *>(p.widget)->setColor(text);
	else if (p.kind == "shape")
		static_cast<QComboBox *>(p.widget)->setCurrentText(text);
	else if (p.kind == "bool")
		static_cast<QCheckBox *>(p.widget)->setChecked(text == "true");
	else if (p.kind == "string")
		static_cast<QLineEdit *>(p.widget)->setText(text);
	else
		static_cast<QDoubleSpinBox *>(p.widget)->setValue(text.toDouble());
}

/* An editor changed (by hand or through a reset): note it, refresh the reset button, redraw the preview. */
void ThemeEditor::touch(const QString &element, const QString &name)
{
	if (loading_)
		return;
	Prop &p = props_[element][name];
	p.touched = true;
	updateReset(p);
	emit edited();
	schedulePreview();
}

void ThemeEditor::updateReset(Prop &p)
{
	if (!p.reset)
		return;
	bool differs = valueOf(p) != p.defaultV;
	p.reset->setEnabled(differs);
	p.reset->setToolTip(differs ? tr("Reset to the built-in value (%1)").arg(p.defaultV)
				    : tr("Already the built-in value"));
}

void ThemeEditor::resetElement(const QString &element)
{
	loading_ = true;   /* one preview and one notice for the lot */
	for (auto p = props_[element].begin(); p != props_[element].end(); ++p) {
		setValue(*p, p->defaultV);
		p->touched = true;
		updateReset(*p);
	}
	loading_ = false;
	emit edited();
	schedulePreview();
}

void ThemeEditor::resetAll()
{
	loading_ = true;
	for (auto e = props_.begin(); e != props_.end(); ++e)
		for (auto p = e->begin(); p != e->end(); ++p) {
			setValue(*p, p->defaultV);
			p->touched = true;
			updateReset(*p);
		}
	loading_ = false;
	emit edited();
	schedulePreview();
}

/* Runs rei-wm with a flag that prints theme values for `configText`. */
bool ThemeEditor::fetch(const QString &flag, const QString &configText, Values *out, QString *error) const
{
	QTemporaryFile tmp(QDir::tempPath() + "/rei-theme-XXXXXX.toml");
	if (!tmp.open() || tmp.write(configText.toUtf8()) < 0) {
		if (error)
			*error = tr("Cannot write a temporary file.");
		return false;
	}
	tmp.flush();
	QProcess p;
	p.start(binary_, { flag, "--config", tmp.fileName() });
	if (!p.waitForFinished(10000) || p.exitCode() != 0) {
		if (error)
			*error = tr("rei-wm could not describe the theme (%1)")
					 .arg(QString::fromUtf8(p.readAllStandardError()).trimmed());
		return false;
	}
	const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
	for (const QString &line : lines) {
		int eq = line.indexOf('=');
		int dot = line.indexOf('.');
		if (eq < 0 || dot < 0 || dot > eq)
			continue;
		(*out)[line.left(dot)][line.mid(dot + 1, eq - dot - 1)] = line.mid(eq + 1);
	}
	return true;
}

void ThemeEditor::setBinary(const QString &path)
{
	binary_ = path;
}

bool ThemeEditor::load(const QString &configText, QString *error)
{
	body_->setEnabled(false);
	QString why;
	Values effective, defaults;
	if (binary_.isEmpty()) {
		why = tr("The theme editor needs the rei-wm program to tell it the theme and to draw the preview, "
			 "and it was not found next to this program, in ../src, in $REI_WM_BIN or on PATH.");
	} else if (!fetch("--print-theme", configText, &effective, &why) ||
		   !fetch("--print-theme-defaults", configText, &defaults, &why)) {
		why += tr("\n(Using %1. If that rei-wm is older than this settings program, rebuild or reinstall it.)")
			       .arg(binary_);
	}
	if (!why.isEmpty()) {
		notice_->setText(why);
		noticeBox_->show();
		if (error)
			*error = why;
		return false;
	}
	noticeBox_->hide();
	loading_ = true;
	for (auto e = props_.begin(); e != props_.end(); ++e)
		for (auto p = e->begin(); p != e->end(); ++p) {
			if (effective.contains(e.key()) && effective[e.key()].contains(p->name))
				setValue(*p, effective[e.key()][p->name]);
			p->loaded = valueOf(*p);   /* what the widget normalised it to */
			p->defaultV = p->loaded;
			if (defaults.contains(e.key()) && defaults[e.key()].contains(p->name)) {
				Prop tmp = *p;   /* normalise the default the same way */
				setValue(tmp, defaults[e.key()][p->name]);
				p->defaultV = valueOf(tmp);
				setValue(*p, effective[e.key()][p->name]);
			}
			p->touched = false;
			updateReset(*p);
		}
	loading_ = false;
	body_->setEnabled(true);
	schedulePreview();
	return true;
}

bool ThemeEditor::changed() const
{
	for (auto e = props_.begin(); e != props_.end(); ++e)
		for (auto p = e->begin(); p != e->end(); ++p)
			if (valueOf(*p) != p->loaded)
				return true;
	return false;
}

void ThemeEditor::apply(TomlDoc &doc) const
{
	for (auto e = props_.begin(); e != props_.end(); ++e) {
		for (auto p = e->begin(); p != e->end(); ++p) {
			QString now = valueOf(*p);
			if (now == p->loaded)
				continue;   /* untouched: the file keeps its own line (or the built-in look) */
			if (now == p->defaultV) {
				/* Back to the built-in value: drop the override instead of pinning the value, so the
				 * element keeps following the toolbar colours. */
				doc.remove("theme." + e.key(), p->name);
				doc.pruneEmpty("theme." + e.key());
				continue;
			}
			QVariant v;
			if (p->kind == "number")
				v = now.toDouble();
			else if (p->kind == "bool")
				v = now == "true";
			else
				v = now;
			doc.set("theme." + e.key(), p->name, v);
		}
	}
}

void ThemeEditor::markSaved()
{
	for (auto e = props_.begin(); e != props_.end(); ++e)
		for (auto p = e->begin(); p != e->end(); ++p) {
			p->loaded = valueOf(*p);
			p->touched = false;
		}
}

void ThemeEditor::baseColorsChanged()
{
	Values defaults;
	if (binary_.isEmpty() || !fetch("--print-theme-defaults", text_(), &defaults, nullptr))
		return;
	loading_ = true;
	for (auto e = props_.begin(); e != props_.end(); ++e)
		for (auto p = e->begin(); p != e->end(); ++p) {
			if (!defaults.contains(e.key()) || !defaults[e.key()].contains(p->name))
				continue;
			QString current = valueOf(*p);
			Prop probe = *p;
			setValue(probe, defaults[e.key()][p->name]);   /* (sets the shared widget; put it back below) */
			QString fresh = valueOf(probe);
			setValue(*p, current);
			bool follows = !p->touched && p->loaded == p->defaultV && current == p->defaultV;
			p->defaultV = fresh;
			if (follows) {   /* the built-in look moved, and this value was the built-in look: it moves too */
				setValue(*p, fresh);
				p->loaded = fresh;
			}
			updateReset(*p);
		}
	loading_ = false;
	schedulePreview();
}

void ThemeEditor::schedulePreview()
{
	if (body_->isEnabled() || loading_)
		timer_->start();
}

void ThemeEditor::startPreview()
{
	if (binary_.isEmpty())
		return;
	if (process_) {   /* one at a time; run again afterwards */
		pending_ = true;
		return;
	}
	QTemporaryFile cfg(QDir::tempPath() + "/rei-preview-XXXXXX.toml");
	cfg.setAutoRemove(false);
	if (!cfg.open())
		return;
	cfg.write(text_().toUtf8());
	cfg.close();
	previewPath_ = cfg.fileName() + ".png";
	previewCfg_ = cfg.fileName();
	process_ = new QProcess(this);
	QString cfgPath = cfg.fileName();
	connect(process_, &QProcess::finished, this, [this, cfgPath] {
		QPixmap pm(previewPath_);
		if (!pm.isNull()) {
			preview_->setPixmap(pm);
			preview_->resize(pm.size());
			note_->setText(tr("Preview (drawn by rei-wm, so it matches what you will see)"));
		} else {
			note_->setText(tr("The preview could not be drawn (the settings may be invalid)."));
		}
		QFile::remove(previewPath_);
		QFile::remove(cfgPath);
		process_->deleteLater();
		process_ = nullptr;
		if (pending_) {
			pending_ = false;
			startPreview();
		}
	});
	process_->start(binary_, { "--theme-preview", previewPath_, "--config", cfgPath });
}

bool ThemeEditor::refreshPreviewNow(QString *error)
{
	timer_->stop();
	while (process_) {   /* let a running one finish */
		process_->waitForFinished(10000);
		QCoreApplication::processEvents();
	}
	startPreview();
	if (process_)
		process_->waitForFinished(15000);
	QCoreApplication::processEvents();
	if (preview_->pixmap().isNull()) {
		if (error)
			*error = tr("no preview");
		return false;
	}
	return true;
}

QSize ThemeEditor::previewSize() const
{
	return preview_->pixmap().size();
}
