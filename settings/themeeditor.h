#ifndef REI_SETTINGS_THEMEEDITOR_H
#define REI_SETTINGS_THEMEEDITOR_H

#include <QMap>
#include <QString>
#include <QVariant>
#include <QWidget>
#include <functional>

#include "tomldoc.h"

class QLabel;
class QListWidget;
class QProcess;
class QStackedWidget;
class QTimer;

/*
 * The theme editor: every value the theme controls, for every element, with a preview.
 *
 * The values shown are the *effective* theme as rei-wm computes it (`rei-wm --print-theme`): its built-in
 * look, derived from the toolbar colours, with the file's [theme.<element>] tables applied. Saving writes
 * only the values the user changed, into those tables. The preview is drawn by rei-wm itself
 * (`rei-wm --theme-preview`), so it is exactly what the compositor will draw, and it follows every edit.
 */
class ThemeEditor : public QWidget {
	Q_OBJECT
public:
	/* `textProvider` returns the config text as it would be saved now (edits included). */
	ThemeEditor(const QString &compositorBinary, std::function<QString()> textProvider,
		    QWidget *parent = nullptr);

	~ThemeEditor() override;

	/* Reads the effective theme of `configText` and shows it. Returns false (with a reason) if rei-wm
	 * could not be asked; the editor is then disabled. */
	bool load(const QString &configText, QString *error = nullptr);
	/* Writes the changed values into `doc`. */
	void apply(TomlDoc &doc) const;
	/* After a save: what is shown is now the baseline. */
	void markSaved();
	bool changed() const;

	/* Draws the preview now (waits for it). For tests. */
	bool refreshPreviewNow(QString *error = nullptr);
	QSize previewSize() const;

signals:
	void edited();

private:
	struct Prop {
		QString name;
		QWidget *widget = nullptr;
		QString kind;        /* "color", "number", "shape", "bool" */
		QString loaded;      /* The effective value as text, at load. */
	};

	void buildPage(const QString &element, bool iconable);
	QString valueOf(const Prop &p) const;
	void setValue(Prop &p, const QString &text);
	void schedulePreview();
	void startPreview();

	QString binary_;
	std::function<QString()> text_;
	QListWidget *list_ = nullptr;
	QStackedWidget *pages_ = nullptr;
	QLabel *preview_ = nullptr, *note_ = nullptr;
	QTimer *timer_ = nullptr;
	QProcess *process_ = nullptr;
	QMap<QString, QMap<QString, Prop>> props_;   /* element -> property -> editor */
	QMap<QString, int> pageOf_;
	bool loading_ = false;
	bool pending_ = false;
	QString previewPath_, previewCfg_;   /* the files of the preview being drawn */
};

#endif
