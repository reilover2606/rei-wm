#ifndef REI_SETTINGS_TOMLDOC_H
#define REI_SETTINGS_TOMLDOC_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

/*
 * A TOML file held as lines, edited in place.
 *
 * The settings app must not destroy the comments and layout of a file people also
 * edit by hand, so it never regenerates the file: it changes the value on an existing
 * `key = value` line (keeping any trailing comment), inserts a missing key after the
 * last key of its section, and only rewrites whole blocks for [[output]]. It understands
 * exactly the subset of TOML that rei-wm's config uses: [tables], [[arrays of tables]],
 * and string / integer / float / boolean values.
 */
class TomlDoc {
public:
	static TomlDoc parse(const QString &text);

	QString text() const;

	bool has(const QString &section, const QString &key) const;
	/* A QString, qlonglong, double or bool; invalid if absent. */
	QVariant get(const QString &section, const QString &key) const;
	void set(const QString &section, const QString &key, const QVariant &value);
	void remove(const QString &section, const QString &key);
	QStringList keys(const QString &section) const;

	/* [[name]] tables, in file order; each is key -> value. */
	QList<QVariantMap> tables(const QString &name) const;
	void setTables(const QString &name, const QList<QVariantMap> &tables);

	/* TOML spelling of a value ("abc", 12, 1.5, true). */
	static QString format(const QVariant &value);

private:
	struct Line {
		enum Kind { Other, Table, ArrayTable, Key } kind = Other;
		QString section;  /* for a table: its name; for a key: its table ("" = top level) */
		QString key;
		int valueStart = 0, valueEnd = 0;  /* span of the value text within the line */
		QVariant value;
	};

	void scan();
	int lastLineOfSection(const QString &section) const;

	QStringList lines_;
	QVector<Line> info_;
};

#endif
