#include "tomldoc.h"

#include <QRegularExpression>

static QString unquoteKey(const QString &k)
{
	if (k.size() >= 2 && k.startsWith('"') && k.endsWith('"'))
		return k.mid(1, k.size() - 2).replace("\\\"", "\"").replace("\\\\", "\\");
	return k;
}

static QString quoteKey(const QString &k)
{
	static const QRegularExpression bare("^[A-Za-z0-9_-]+$");
	if (bare.match(k).hasMatch())
		return k;
	QString q = k;
	q.replace("\\", "\\\\").replace("\"", "\\\"");
	return "\"" + q + "\"";
}

QString TomlDoc::format(const QVariant &v)
{
	switch (v.typeId()) {
	case QMetaType::Bool:
		return v.toBool() ? "true" : "false";
	case QMetaType::Int:
	case QMetaType::LongLong:
	case QMetaType::UInt:
	case QMetaType::ULongLong:
		return QString::number(v.toLongLong());
	case QMetaType::Double:
	case QMetaType::Float: {
		QString s = QString::number(v.toDouble(), 'g', 10);
		if (!s.contains('.') && !s.contains('e') && !s.contains("inf") && !s.contains("nan"))
			s += ".0";
		return s;
	}
	default: {
		QString s = v.toString();
		s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\t", "\\t");
		return "\"" + s + "\"";
	}
	}
}

TomlDoc TomlDoc::parse(const QString &text)
{
	TomlDoc d;
	d.lines_ = text.split('\n');
	if (!d.lines_.isEmpty() && d.lines_.last().isEmpty())
		d.lines_.removeLast();  /* the trailing newline is re-added by text() */
	d.scan();
	return d;
}

QString TomlDoc::text() const
{
	return lines_.join('\n') + "\n";
}

void TomlDoc::scan()
{
	info_.clear();
	info_.resize(lines_.size());

	static const QRegularExpression header(R"(^\s*\[\s*([^\[\]]+?)\s*\]\s*(#.*)?$)");
	static const QRegularExpression arrayHeader(R"(^\s*\[\[\s*([^\[\]]+?)\s*\]\]\s*(#.*)?$)");
	static const QRegularExpression keyLine(R"re(^\s*("(?:[^"\\]|\\.)*"|[A-Za-z0-9_.-]+)\s*=\s*)re");

	QString section;
	QHash<QString, int> arrayCount;
	for (int i = 0; i < lines_.size(); i++) {
		const QString &l = lines_[i];
		Line &li = info_[i];
		QRegularExpressionMatch m;
		if ((m = arrayHeader.match(l)).hasMatch()) {
			QString name = m.captured(1);
			int n = arrayCount[name]++;
			section = name + "#" + QString::number(n);
			li.kind = Line::ArrayTable;
			li.section = section;
		} else if ((m = header.match(l)).hasMatch()) {
			section = m.captured(1);
			li.kind = Line::Table;
			li.section = section;
		} else if ((m = keyLine.match(l)).hasMatch()) {
			li.kind = Line::Key;
			li.section = section;
			li.key = unquoteKey(m.captured(1));
			int start = m.capturedEnd(0), end = start;
			if (start < l.size() && l[start] == '"') {
				end = start + 1;
				while (end < l.size() && l[end] != '"')
					end += l[end] == '\\' ? 2 : 1;
				end = qMin(end + 1, (int)l.size());
				QString raw = l.mid(start + 1, end - start - 2);
				raw.replace("\\n", "\n").replace("\\t", "\t").replace("\\\"", "\"").replace("\\\\", "\\");
				li.value = raw;
			} else {
				end = l.indexOf('#', start);
				if (end < 0)
					end = l.size();
				QString raw = l.mid(start, end - start).trimmed();
				end = start + raw.size();
				bool ok = false;
				if (raw == "true" || raw == "false") {
					li.value = raw == "true";
				} else if (qlonglong n = raw.toLongLong(&ok); ok) {
					li.value = n;
				} else if (double dv = raw.toDouble(&ok); ok) {
					li.value = dv;
				}
			}
			li.valueStart = start;
			li.valueEnd = end;
		}
	}
}

bool TomlDoc::has(const QString &section, const QString &key) const
{
	return get(section, key).isValid();
}

QVariant TomlDoc::get(const QString &section, const QString &key) const
{
	for (const Line &li : info_)
		if (li.kind == Line::Key && li.section == section && li.key == key)
			return li.value;
	return {};
}

QStringList TomlDoc::keys(const QString &section) const
{
	QStringList out;
	for (const Line &li : info_)
		if (li.kind == Line::Key && li.section == section)
			out << li.key;
	return out;
}

int TomlDoc::lastLineOfSection(const QString &section) const
{
	int last = -1;
	for (int i = 0; i < info_.size(); i++) {
		const Line &li = info_[i];
		if ((li.kind == Line::Table || li.kind == Line::ArrayTable) && li.section == section && last < i)
			last = i;
		if (li.kind == Line::Key && li.section == section)
			last = i;
	}
	return last;
}

void TomlDoc::set(const QString &section, const QString &key, const QVariant &value)
{
	for (int i = 0; i < info_.size(); i++) {
		const Line &li = info_[i];
		if (li.kind == Line::Key && li.section == section && li.key == key) {
			lines_[i] = lines_[i].left(li.valueStart) + format(value) + lines_[i].mid(li.valueEnd);
			scan();
			return;
		}
	}
	QString newLine = quoteKey(key) + " = " + format(value);
	int after = lastLineOfSection(section);
	if (after >= 0) {
		lines_.insert(after + 1, newLine);
	} else if (section.isEmpty()) {
		/* Top-level keys go before the first table. */
		int firstTable = lines_.size();
		for (int i = 0; i < info_.size(); i++)
			if (info_[i].kind == Line::Table || info_[i].kind == Line::ArrayTable) {
				firstTable = i;
				break;
			}
		lines_.insert(firstTable, newLine);
	} else {
		if (!lines_.isEmpty() && !lines_.last().trimmed().isEmpty())
			lines_ << "";
		lines_ << "[" + section + "]" << newLine;
	}
	scan();
}

void TomlDoc::remove(const QString &section, const QString &key)
{
	for (int i = 0; i < info_.size(); i++) {
		const Line &li = info_[i];
		if (li.kind == Line::Key && li.section == section && li.key == key) {
			lines_.removeAt(i);
			scan();
			return;
		}
	}
}

QList<QVariantMap> TomlDoc::tables(const QString &name) const
{
	QList<QVariantMap> out;
	QString prefix = name + "#";
	for (const Line &li : info_) {
		if (li.kind == Line::ArrayTable && li.section.startsWith(prefix))
			out << QVariantMap();
		else if (li.kind == Line::Key && li.section.startsWith(prefix)) {
			int idx = li.section.mid(prefix.size()).toInt();
			if (idx < out.size())
				out[idx][li.key] = li.value;
		}
	}
	return out;
}

void TomlDoc::setTables(const QString &name, const QList<QVariantMap> &tables)
{
	/* Drop every existing block: its header, its keys and the blank line after it. */
	QString prefix = name + "#";
	for (int i = info_.size() - 1; i >= 0; i--) {
		const Line &li = info_[i];
		bool mine = li.section.startsWith(prefix) && (li.kind == Line::ArrayTable || li.kind == Line::Key);
		if (!mine)
			continue;
		lines_.removeAt(i);
		if (li.kind == Line::ArrayTable && i < lines_.size() && lines_[i].trimmed().isEmpty())
			lines_.removeAt(i);
	}
	while (!lines_.isEmpty() && lines_.last().trimmed().isEmpty())
		lines_.removeLast();

	for (const QVariantMap &t : tables) {
		lines_ << "" << "[[" + name + "]]";
		/* A readable order: identity first, then geometry, then the rest. */
		static const QStringList preferred = { "name", "mode", "width", "height", "refresh", "scale",
						       "x", "y", "enabled" };
		QStringList order = preferred;
		for (const QString &k : t.keys())
			if (!order.contains(k))
				order << k;
		for (const QString &k : order)
			if (t.value(k).isValid())
				lines_ << quoteKey(k) + " = " + format(t.value(k));
	}
	scan();
}
