#ifndef REI_SETTINGS_MAINWINDOW_H
#define REI_SETTINGS_MAINWINDOW_H

#include <QMainWindow>
#include <QVariant>
#include <QVector>

#include "tomldoc.h"

class QLabel;
class QTableWidget;
class QWidget;
class ThemeEditor;

/*
 * The settings window. Every simple option is a "field": a widget bound to a
 * [section] key of config.toml. Saving writes only the fields the user changed (into
 * the existing file, comments intact), checks the result with `rei-wm --check-config`,
 * and replaces the file atomically; the running compositor notices and reloads.
 */
class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(const QString &configPath, const QString &compositorBinary = QString(),
			    QWidget *parent = nullptr);

	/* Re-reads the file. */
	bool load(QString *error = nullptr);
	/* Validates and writes it. On failure returns false and says why. */
	bool save(QString *error = nullptr);

	bool dirty() const;
	QString configPath() const { return path_; }

private slots:
	void onSaveClicked();
	void onRevertClicked();
	void markDirty();

private:
	enum Type { Bool, Int, Double, String, Choice, Color, Path, Font, Dir };
	struct Field {
		QString section, key;
		Type type;
		QWidget *widget;
		QVariant fallback;  /* Shown when the file does not set it. */
		QVariant loaded;    /* What the widget held after load(). */
	};

	QWidget *addField(class QFormLayout *form, const QString &label, const QString &section,
			  const QString &key, Type type, const QVariant &fallback,
			  const QStringList &choices = {}, int min = 0, int max = 0, double step = 1.0,
			  const QString &tooltip = QString());
	QVariant fieldValue(const Field &f) const;
	void setFieldValue(Field &f, const QVariant &v);

	QWidget *buildGeneralTab();
	QWidget *buildLayoutTab();
	QWidget *buildInputTab();
	QWidget *buildToolbarTab();
	QWidget *buildLookTab();
	QWidget *buildKeysTab();
	QWidget *buildOutputsTab();
	QWidget *buildStartMenuTab();
	QWidget *buildAudioTab();
	QWidget *buildMessagesTab();
	QWidget *buildMusicTab();
	QWidget *buildThemeTab();
	QWidget *buildCursorTab();
	void updateCursorPreview();
	QStringList installedCursorThemes() const;
	TomlDoc buildDoc() const;   /* The document as it would be saved now. */

	void loadKeys();
	void loadOutputs();
	void loadShortcuts();
	QList<QVariantMap> shortcutTable() const;
	QMap<QString, QString> keyTable() const;
	QList<QVariantMap> outputTable() const;
	QString findCompositor() const;
	bool validate(const QString &text, QString *error);

	QString path_, binary_;   /* (binary_ is also used by the theme editor) */
	TomlDoc doc_;
	QVector<Field> fields_;
	QTableWidget *keys_ = nullptr, *outputs_ = nullptr, *shortcuts_ = nullptr;
	QLabel *status_ = nullptr, *cursorPreview_ = nullptr;
	ThemeEditor *theme_ = nullptr;
	QMap<QString, QString> loadedKeys_;
	QList<QVariantMap> loadedOutputs_, loadedShortcuts_;
	bool loading_ = false;
	bool dirty_ = false;
};

#endif
