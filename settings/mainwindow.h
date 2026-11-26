#ifndef REI_SETTINGS_MAINWINDOW_H
#define REI_SETTINGS_MAINWINDOW_H

#include <QMainWindow>
#include <QVariant>
#include <QVector>

#include "tomldoc.h"

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
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

	/* Adds a wallpaper group made of these: one picture, several pictures, or a folder (its pictures). The
	 * Wallpaper tab's buttons call this; it is public for tests. */
	void addWallpapers(const QStringList &paths);

	/* Theme files: the toolbar colours and the [theme.*] tables that differ from the built-in look, as TOML (the same
	 * lines config.toml uses). They live in <config dir>/themes by default. Public for tests; the buttons call them. */
	QString themesDir() const;
	bool exportTheme(const QString &file, QString *error = nullptr);
	/* Replaces the whole current look with the file's. `warnings` gets what was skipped. */
	bool importTheme(const QString &file, QStringList *warnings = nullptr, QString *error = nullptr);

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
	QWidget *buildWallpaperTab();
	void loadWallpapers();
	QList<QVariantMap> wallpaperTable() const;
	QString wallpaperProblem() const;
	void showWallpaperFiles();   /* the picture list under the table: the pictures of the selected group */
	void setWallpaperPaths(int row, const QStringList &paths);
	void setWallpaperRow(int row, const QString &name, const QStringList &paths, int interval, bool any, const QString &workspaces);
	QWidget *buildKeysTab();
	QWidget *buildOutputsTab();
	QWidget *buildStartMenuTab();
	QWidget *buildAudioTab();
	QWidget *buildMessagesTab();
	QWidget *buildMusicTab();
	QWidget *buildThemeTab();
	void refreshThemeList();
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
	QTableWidget *keys_ = nullptr, *outputs_ = nullptr, *shortcuts_ = nullptr, *wallpapers_ = nullptr;
	QListWidget *wpFiles_ = nullptr;
	QPushButton *wpFileAdd_ = nullptr, *wpFolderAdd_ = nullptr, *wpFileDel_ = nullptr;
	class QButtonGroup *anyGroup_ = nullptr;   /* the "any" radio buttons: at most one group is the any group */
	QLabel *status_ = nullptr, *cursorPreview_ = nullptr;
	ThemeEditor *theme_ = nullptr;
	QWidget *baseColors_[3] = {};   /* toolbar background, foreground, highlight */
	QComboBox *themeList_ = nullptr;
	QMap<QString, QString> loadedKeys_;
	QList<QVariantMap> loadedOutputs_, loadedShortcuts_, loadedWallpapers_;
	bool loading_ = false;
	bool dirty_ = false;
};

#endif
