#ifndef REI_SETTINGS_COLORBUTTON_H
#define REI_SETTINGS_COLORBUTTON_H

#include <QColor>
#include <QColorDialog>
#include <QPushButton>
#include <functional>

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


#endif
