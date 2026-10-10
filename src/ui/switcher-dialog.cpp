/*
EnhancedIRL OBS Switcher
Copyright (C) 2026 EnhancedIRL

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * The plugin's windows. The main one is a three-step checklist that always shows
 * what is missing: link this OBS, use the switcher's stream service, and be ready
 * to move the stream. The other one tells the user that the stream moved to
 * another OBS. Everything else lives in the web panel (My OBS).
 */

#include "switcher-dialog.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

extern "C" {
#include <plugin-support.h>
#include "../agent/agent.hpp"
#include "../managed.h"
#include "../switcher-service.h"
}

namespace {

/* Only the main action of each state is blue; everything else follows the OBS theme. */
constexpr const char *PRIMARY_STYLE = "QPushButton { background-color: #465FFF; border: 1px solid #465FFF;"
				      " color: #FFFFFF; font-weight: bold; padding-left: 14px; padding-right: 14px; }"
				      "QPushButton:hover { background-color: #5A70FF; border-color: #5A70FF; }"
				      "QPushButton:pressed { background-color: #3A50E6; border-color: #3A50E6; }"
				      "QPushButton:focus { border-color: #A3B0FF; }"
				      "QPushButton:disabled { background-color: rgba(70, 95, 255, 0.35);"
				      " border-color: transparent; color: rgba(255, 255, 255, 0.55); }";
constexpr const char *GREEN = "#25A231";
constexpr const char *MUTED = "rgb(153, 153, 153)";

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString error_text(const char *error)
{
	static const struct {
		const char *key;
		const char *text;
	} map[] = {
		{"revoked", "Error.Revoked"},
		{"not_allowed", "Error.NotAllowed"},
		{"replaced", "Error.Replaced"},
		{"upgrade", "Error.Upgrade"},
		{"limit_reached", "Error.LimitReached"},
		{"email_not_verified", "Error.EmailNotVerified"},
		{"expired", "Error.Expired"},
		{"network", "Error.Network"},
		{"too_many", "Error.TooMany"},
		{"unexpected", "Error.Unexpected"},
	};
	for (const auto &m : map)
		if (strcmp(error, m.key) == 0)
			return T(m.text);
	return T("Error.Unexpected");
}

/* The user guide, in OBS's language when it has one. */
QString docs_url()
{
	const char *locale = obs_get_locale();
	QString lang = "en";
	if (locale && strncmp(locale, "es", 2) == 0)
		lang = "es";
	else if (locale && strncmp(locale, "fr", 2) == 0)
		lang = "fr";
	return QStringLiteral("https://docs.enhancedirl.com/go/%1/my-obs").arg(lang);
}

/* OBS's own text classes (themes of OBS 30 and later): muted, warning... */
void set_class(QWidget *w, const char *cls)
{
	if (w->property("class").toString() == QLatin1String(cls))
		return;
	w->setProperty("class", cls);
	w->style()->unpolish(w);
	w->style()->polish(w);
}

/* Text that may come from the backend is always plain text, never HTML. */
QLabel *text_label(QWidget *parent, const char *cls = nullptr)
{
	auto *l = new QLabel(parent);
	l->setTextFormat(Qt::PlainText);
	l->setWordWrap(true);
	if (cls)
		l->setProperty("class", cls);
	return l;
}

/* A small, quiet link (Cancel, Unlink...): the theme's muted text, underlined. */
QLabel *link_label(QWidget *parent, const QString &text, std::function<void()> on_click)
{
	auto *l = new QLabel(parent);
	l->setTextFormat(Qt::RichText);
	l->setText(QStringLiteral("<a href=\"#\" style=\"color: %1; text-decoration: underline;\">%2</a>")
			   .arg(MUTED, text.toHtmlEscaped()));
	l->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
	l->setCursor(Qt::PointingHandCursor);
	QObject::connect(l, &QLabel::linkActivated, l, [on_click](const QString &) { on_click(); });
	return l;
}

QPushButton *primary_button(QWidget *parent, const QString &text)
{
	auto *b = new QPushButton(text, parent);
	b->setStyleSheet(PRIMARY_STYLE);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}

enum class Mark { Pending, Current, Done };

/* Step marker: a numbered circle, or a green check once the step is done. */
QPixmap step_pixmap(const QWidget *w, Mark mark, int number)
{
	const int size = 22;
	const qreal dpr = w->devicePixelRatioF();
	QPixmap pm(QSize(size, size) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);

	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF circle(1.0, 1.0, size - 2.0, size - 2.0);
	if (mark == Mark::Done) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(GREEN));
		p.drawEllipse(circle);
		QPen pen(Qt::white, 2.2);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		QPainterPath check;
		check.moveTo(6.5, 11.5);
		check.lineTo(9.6, 14.6);
		check.lineTo(15.5, 8.2);
		p.drawPath(check);
	} else {
		const QColor c = mark == Mark::Current ? w->palette().color(QPalette::WindowText)
						       : QColor(128, 128, 128);
		p.setPen(QPen(c, 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));
		QFont f = w->font();
		f.setBold(true);
		f.setPixelSize(11);
		p.setFont(f);
		p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, QString::number(number));
	}
	return pm;
}

/* Window icon: two opposite arrows, the stream going from one OBS to another. */
QPixmap header_pixmap(const QWidget *w)
{
	const int size = 28;
	const qreal dpr = w->devicePixelRatioF();
	QPixmap pm(QSize(size, size) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);

	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(128, 128, 128, 60));
	p.drawRoundedRect(QRectF(0, 0, size, size), 6, 6);
	QPen pen(w->palette().color(QPalette::WindowText), 2.0);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	QPainterPath arrows;
	arrows.moveTo(7, 10.5);
	arrows.lineTo(20, 10.5);
	arrows.moveTo(16.5, 7);
	arrows.lineTo(20, 10.5);
	arrows.lineTo(16.5, 14);
	arrows.moveTo(21, 17.5);
	arrows.lineTo(8, 17.5);
	arrows.moveTo(11.5, 14);
	arrows.lineTo(8, 17.5);
	arrows.lineTo(11.5, 21);
	p.drawPath(arrows);
	return pm;
}

/* The pairing code, one character per box, with the dash in the middle. */
class CodeBoxes : public QWidget {
public:
	explicit CodeBoxes(QWidget *parent) : QWidget(parent)
	{
		auto *row = new QHBoxLayout(this);
		row->setContentsMargins(0, 4, 0, 4);
		row->setSpacing(6);
		const QString mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
		const QString box = QStringLiteral("QLabel { font-family: '%1'; font-size: 20pt; font-weight: bold;"
						   " border: 1px solid rgba(128, 128, 128, 0.6); border-radius: 6px;"
						   " background-color: rgba(128, 128, 128, 0.12); padding: 0px; }")
					    .arg(mono);
		for (int i = 0; i < 8; i++) {
			if (i == 4) {
				dash_ = new QLabel(QStringLiteral("-"), this);
				dash_->setStyleSheet(QStringLiteral("QLabel { font-family: '%1'; font-size: 20pt;"
								    " font-weight: bold; padding: 0px 2px; }")
							     .arg(mono));
				row->addWidget(dash_);
			}
			cells_[i] = new QLabel(this);
			cells_[i]->setTextFormat(Qt::PlainText);
			cells_[i]->setAlignment(Qt::AlignCenter);
			cells_[i]->setFixedSize(34, 44);
			cells_[i]->setStyleSheet(box);
			row->addWidget(cells_[i]);
		}
		/* Codes in any other shape (not expected) still show, just in one piece. */
		whole_ = new QLabel(this);
		whole_->setTextFormat(Qt::PlainText);
		whole_->setStyleSheet(
			QStringLiteral("QLabel { font-family: '%1'; font-size: 20pt; font-weight: bold; }").arg(mono));
		whole_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		row->addWidget(whole_);
		row->addStretch();
	}

	void set_code(const QString &code)
	{
		if (code == code_)
			return;
		code_ = code;
		QString chars = code;
		chars.remove(QLatin1Char('-'));
		const bool boxes = chars.size() == 8;
		for (int i = 0; i < 8; i++) {
			cells_[i]->setVisible(boxes);
			cells_[i]->setText(boxes ? chars.mid(i, 1) : QString());
		}
		dash_->setVisible(boxes);
		whole_->setVisible(!boxes);
		whole_->setText(boxes ? QString() : code);
		setAccessibleName(code);
	}

private:
	QLabel *cells_[8] = {};
	QLabel *dash_ = nullptr;
	QLabel *whole_ = nullptr;
	QString code_;
};

/* One step of the checklist: marker, title and whatever the step shows below. */
struct Step {
	QLabel *mark = nullptr;
	QLabel *title = nullptr;
	QVBoxLayout *body = nullptr;
	int number = 0;
	int shown = -1; /* Mark last drawn */

	void set(const QWidget *w, Mark m, const QString &text)
	{
		if ((int)m != shown) {
			mark->setPixmap(step_pixmap(w, m, number));
			shown = (int)m;
		}
		title->setText(text);
		set_class(title, m == Mark::Pending ? "text-muted" : "");
	}
};

QString time_of_day(long long at_ms)
{
	return QDateTime::fromMSecsSinceEpoch(at_ms).toString(QStringLiteral("HH:mm"));
}

class SwitcherDialog : public QDialog {
public:
	explicit SwitcherDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle("EnhancedIRL Switcher");
		setMinimumWidth(480);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(20, 18, 20, 16);
		layout->setSpacing(14);

		/* Header: name on the left, a short state on the right. */
		auto *header = new QHBoxLayout();
		header->setSpacing(10);
		auto *icon = new QLabel(this);
		icon->setPixmap(header_pixmap(this));
		auto *name = new QLabel(QStringLiteral("EnhancedIRL Switcher"), this);
		name->setStyleSheet(QStringLiteral("QLabel { font-weight: bold; }"));
		name->setProperty("class", "text-large");
		badge_ = new QLabel(this);
		badge_->setTextFormat(Qt::PlainText);
		header->addWidget(icon);
		header->addWidget(name);
		header->addStretch();
		header->addWidget(badge_);
		layout->addLayout(header);

		/* Everything done: say so above the list. */
		ready_ = new QFrame(this);
		ready_->setObjectName("eirlReady");
		ready_->setStyleSheet(
			QStringLiteral("QFrame#eirlReady { background-color: rgba(37, 162, 49, 0.14);"
				       " border: 1px solid rgba(37, 162, 49, 0.55); border-radius: 6px; }"
				       "QFrame#eirlReady QLabel { background: transparent; border: none; }"));
		auto *readyRow = new QHBoxLayout(ready_);
		readyRow->setContentsMargins(12, 10, 12, 10);
		readyRow->setSpacing(10);
		auto *readyMark = new QLabel(ready_);
		readyMark->setPixmap(step_pixmap(this, Mark::Done, 0));
		readyMark->setAlignment(Qt::AlignTop);
		auto *readyText = new QLabel(ready_);
		readyText->setTextFormat(Qt::RichText);
		readyText->setWordWrap(true);
		readyText->setText(QStringLiteral("<b>%1</b> · %2")
					   .arg(T("Ready.Title").toHtmlEscaped(), T("Ready.Text").toHtmlEscaped()));
		readyRow->addWidget(readyMark, 0, Qt::AlignTop);
		readyRow->addWidget(readyText, 1);
		layout->addWidget(ready_);

		auto *steps = new QVBoxLayout();
		steps->setSpacing(0);
		build_step(steps, step1_, 1);
		steps->addWidget(separator());
		build_step(steps, step2_, 2);
		steps->addWidget(separator());
		build_step(steps, step3_, 3);
		layout->addLayout(steps);

		build_step1();
		build_step2();
		build_step3();

		layout->addStretch();

		/* Footer: help and version on the left; when ready, the way to My OBS. */
		auto *footer = new QHBoxLayout();
		footer->setSpacing(12);
		how_ = link_label(this, T("Footer.HowItWorks"), [] { QDesktopServices::openUrl(QUrl(docs_url())); });
		auto *version = text_label(this, "text-muted");
		version->setWordWrap(false);
		version->setText(QStringLiteral("v%1").arg(QString::fromUtf8(PLUGIN_VERSION)));
		close_ = new QPushButton(T("Button.Close"), this);
		connect(close_, &QPushButton::clicked, this, &QDialog::close);
		openPanel_ = primary_button(this, T("Button.OpenPanel"));
		connect(openPanel_, &QPushButton::clicked, [] { eirl_agent_open_panel(); });
		footer->addWidget(how_);
		footer->addWidget(version);
		footer->addStretch();
		footer->addWidget(close_);
		footer->addWidget(openPanel_);
		layout->addLayout(footer);

		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, [this] { refresh(); });
		timer_->start(500);
		refresh();
	}

private:
	QFrame *separator()
	{
		auto *line = new QFrame(this);
		line->setFixedHeight(1);
		line->setStyleSheet(
			QStringLiteral("QFrame { background-color: rgba(128, 128, 128, 0.25); border: none; }"));
		return line;
	}

	void build_step(QVBoxLayout *into, Step &step, int number)
	{
		auto *grid = new QGridLayout();
		grid->setContentsMargins(0, 12, 0, 12);
		grid->setHorizontalSpacing(12);
		grid->setVerticalSpacing(6);
		step.number = number;
		step.mark = new QLabel(this);
		step.mark->setFixedSize(22, 22);
		step.title = text_label(this);
		step.title->setStyleSheet(QStringLiteral("QLabel { font-weight: bold; }"));
		step.body = new QVBoxLayout();
		step.body->setSpacing(8);
		grid->addWidget(step.mark, 0, 0, Qt::AlignTop);
		grid->addWidget(step.title, 0, 1);
		grid->addLayout(step.body, 1, 1);
		grid->setColumnStretch(1, 1);
		into->addLayout(grid);
	}

	void build_step1()
	{
		s1text_ = text_label(this, "text-muted");
		link_ = primary_button(this, T("Button.Link"));
		connect(link_, &QPushButton::clicked, [] { eirl_agent_begin_pairing(); });
		s1wait_ = text_label(this);
		code_ = new CodeBoxes(this);
		expires_ = text_label(this, "text-muted");
		reopen_ = new QPushButton(T("Button.Reopen"), this);
		connect(reopen_, &QPushButton::clicked, [] { eirl_agent_reopen_browser(); });
		cancel_ = link_label(this, T("Link.Cancel"), [] { eirl_agent_cancel_pairing(); });
		account_ = text_label(this, "text-muted");
		unlink_ = link_label(this, T("Link.Unlink"), [this] { confirm_unlink(); });
		error_ = text_label(this, "text-warning");

		auto *linkRow = new QHBoxLayout();
		linkRow->addWidget(link_);
		linkRow->addStretch();
		auto *pairRow = new QHBoxLayout();
		pairRow->setSpacing(14);
		pairRow->addWidget(reopen_);
		pairRow->addWidget(cancel_);
		pairRow->addStretch();
		auto *accountRow = new QHBoxLayout();
		accountRow->setSpacing(10);
		accountRow->addWidget(account_);
		accountRow->addWidget(unlink_);
		accountRow->addStretch();

		step1_.body->addWidget(s1text_);
		step1_.body->addLayout(linkRow);
		step1_.body->addWidget(s1wait_);
		step1_.body->addWidget(code_);
		step1_.body->addWidget(expires_);
		step1_.body->addLayout(pairRow);
		step1_.body->addLayout(accountRow);
		step1_.body->addWidget(error_);
	}

	void build_step2()
	{
		s2text_ = text_label(this, "text-muted");
		useService_ = primary_button(this, T("Button.UseService"));
		connect(useService_, &QPushButton::clicked, [this] { use_switcher_service(); });
		auto *row = new QHBoxLayout();
		row->addWidget(useService_);
		row->addStretch();
		step2_.body->addWidget(s2text_);
		step2_.body->addLayout(row);
	}

	void build_step3()
	{
		s3text_ = text_label(this, "text-muted");
		step3_.body->addWidget(s3text_);
	}

	void refresh()
	{
		eirl_agent_state s;
		eirl_agent_get_state(&s);
		const bool managed = EIRL_MANAGED_BUILD;

		obs_service_t *service = obs_frontend_get_streaming_service();
		const bool ours = service && switcher_service_platform(service) != nullptr;
		const bool streaming = obs_frontend_streaming_active();

		/* Ours but without a key (or server) yet: it cannot take part in a switch. */
		const bool hasKey = ours && obs_service_can_try_to_connect(service);
		const bool done1 = s.linked;
		const bool done2 = ours && hasKey;
		const bool done3 = done1 && done2 && s.connected;
		const bool ready = done1 && done2 && done3;
		const bool pairing = !managed && !s.linked && s.pairing;
		const bool hasCode = pairing && s.code[0];

		/* Header */
		if (!s.linked)
			set_badge(T("Header.NotLinked"), "rgba(128, 128, 128, 0.20)", MUTED);
		else if (ready)
			set_badge(T("Header.Ready"), "rgba(37, 162, 49, 0.20)", "#4CC85A");
		else
			set_badge(T("Header.StepMissing"), "rgba(229, 168, 46, 0.18)", "#E5A82E");
		ready_->setVisible(ready);

		/* Step 1: link this OBS */
		step1_.set(this, done1 ? Mark::Done : Mark::Current, T(done1 ? "Step1.Done" : "Step1.Title"));
		s1text_->setVisible((!done1 && !pairing) || managed);
		s1text_->setText(T(managed ? "Managed.Linked" : "Step1.Text"));
		link_->setVisible(!managed && !done1 && !pairing);
		s1wait_->setVisible(pairing);
		s1wait_->setText(T(hasCode ? "Step1.Confirm" : "Step1.Requesting"));
		code_->setVisible(hasCode);
		code_->set_code(QString::fromUtf8(s.code));
		expires_->setVisible(hasCode && s.code_seconds_left > 0);
		expires_->setText(T("Step1.Expires")
					  .arg(QStringLiteral("%1:%2")
						       .arg(s.code_seconds_left / 60)
						       .arg(s.code_seconds_left % 60, 2, 10, QLatin1Char('0'))));
		reopen_->setVisible(hasCode);
		cancel_->setVisible(pairing);
		account_->setVisible(!managed && done1 && s.account[0]);
		account_->setText(QString::fromUtf8(s.account));
		unlink_->setVisible(!managed && done1);
		error_->setVisible(s.error[0] && !pairing);
		error_->setText(s.error[0] ? error_text(s.error) : QString());

		/* Step 2: stream with the switcher's service */
		const Mark mark2 = done2 ? Mark::Done : ((done1 || ours) ? Mark::Current : Mark::Pending);
		step2_.set(this, mark2, T(ours ? "Step2.Done" : "Step2.Title"));
		if (done2) {
			const char *display = switcher_service_display_name(service);
			const QString platform = display && *display ? QString::fromUtf8(display)
								     : T("Step2.CustomServer");
			s2text_->setText(T("Step2.DoneText").arg(platform));
		} else if (ours) {
			s2text_->setText(T("Step2.NoKey"));
		} else if (managed) {
			s2text_->setText(T("Managed.ServicePending"));
		} else {
			const QString current = current_service_name(service);
			s2text_->setText(current.isEmpty() ? T("Step2.NotOursUnnamed")
							   : T("Step2.NotOurs").arg(current));
		}
		s2text_->setVisible(done1 || ours || managed);
		/* A Cloud OBS moves to our service by itself (see plugin-main.c). */
		useService_->setVisible(!managed && done1 && !ours);
		useService_->setEnabled(!streaming);
		useService_->setToolTip(streaming ? T("Service.StopFirst") : QString());

		/* Step 3: ready to move the stream */
		const bool connecting = s.linked && !s.connected && !s.error[0];
		const Mark mark3 = done3 ? Mark::Done : ((done1 && done2) ? Mark::Current : Mark::Pending);
		step3_.set(this, mark3, T("Step3.Title"));
		if (done3) {
			s3text_->setText(s.agent_name[0] ? T("Step3.Done").arg(QString::fromUtf8(s.agent_name))
							 : T("Step3.DoneNoName"));
		} else if (connecting) {
			s3text_->setText(T("Step3.Connecting"));
		}
		s3text_->setVisible(done3 || connecting);

		/* Footer */
		how_->setVisible(!managed);
		close_->setVisible(ready && !managed);
		openPanel_->setVisible(ready && !managed);

		fit_to_content();
	}

	void set_badge(const QString &text, const char *bg, const char *fg)
	{
		badge_->setText(text);
		const QString style = QStringLiteral("QLabel { background-color: %1; color: %2; border-radius: 9px;"
						     " padding: 2px 10px; font-weight: bold; }")
					      .arg(QLatin1String(bg), QLatin1String(fg));
		if (badge_->styleSheet() != style)
			badge_->setStyleSheet(style);
	}

	/* Grow or shrink with what the steps show, but only when that changes. */
	void fit_to_content()
	{
		QString shape;
		for (QWidget *w : findChildren<QWidget *>()) {
			const bool visible = w->isVisibleTo(this);
			shape += visible ? QLatin1Char('1') : QLatin1Char('0');
			auto *label = qobject_cast<QLabel *>(w);
			if (label && visible)
				shape += label->text();
		}
		if (shape == shape_)
			return;
		shape_ = shape;
		layout()->activate();
		/* Wrapped text: the height that fits the width the window really has. */
		const int w = isVisible() ? width() : std::max(minimumWidth(), sizeHint().width());
		resize(w, layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(w) : sizeHint().height());
	}

	/* "Twitch", "Custom"... whatever the profile streams with now; empty when
	 * there is nothing better to say than OBS's generic name for its platforms. */
	static QString current_service_name(obs_service_t *service)
	{
		if (!service)
			return QString();
		const char *id = obs_service_get_id(service);
		if (id && strcmp(id, "rtmp_common") == 0) {
			obs_data_t *settings = obs_service_get_settings(service);
			QString name = QString::fromUtf8(obs_data_get_string(settings, "service"));
			obs_data_release(settings);
			return name;
		}
		if (id && strcmp(id, "rtmp_custom") == 0)
			return T("Step2.CustomServer");
		const char *display = id ? obs_service_get_display_name(id) : nullptr;
		return display ? QString::fromUtf8(display) : QString();
	}

	void confirm_unlink()
	{
		QMessageBox box(this);
		box.setIcon(QMessageBox::Question);
		box.setWindowTitle(QStringLiteral("EnhancedIRL Switcher"));
		box.setText(T("Unlink.Title"));
		box.setInformativeText(T("Unlink.Text"));
		QPushButton *yes = box.addButton(T("Link.Unlink"), QMessageBox::DestructiveRole);
		QPushButton *no = box.addButton(T("Link.Cancel"), QMessageBox::RejectRole);
		box.setDefaultButton(no);
		box.exec();
		if (box.clickedButton() == yes) {
			eirl_agent_unlink();
			refresh();
		}
	}

	/* Same platform, server and key the profile already has, in our service. The
	 * key stays inside OBS: it only moves from one service object to another. */
	void use_switcher_service()
	{
		if (obs_frontend_streaming_active())
			return;
		obs_service_t *current = obs_frontend_get_streaming_service();
		if (!current || switcher_service_platform(current))
			return;
		obs_data_t *settings = switcher_service_settings_from(current, nullptr);
		obs_service_t *ours =
			obs_service_create(EIRL_SWITCHER_SERVICE_ID, "default_service", settings, nullptr);
		obs_data_release(settings);
		if (!ours)
			return;
		obs_frontend_set_streaming_service(ours);
		obs_frontend_save_streaming_service();
		obs_service_release(ours);
		blog(LOG_INFO, "[enhancedirl-obs-switcher] stream service switched to EnhancedIRL Switcher");
		refresh();
	}

	QLabel *badge_;
	QFrame *ready_;
	Step step1_, step2_, step3_;
	QLabel *s1text_;
	QPushButton *link_;
	QLabel *s1wait_;
	CodeBoxes *code_;
	QLabel *expires_;
	QPushButton *reopen_;
	QLabel *cancel_;
	QLabel *account_;
	QLabel *unlink_;
	QLabel *error_;
	QLabel *s2text_;
	QPushButton *useService_;
	QLabel *s3text_;
	QLabel *how_;
	QPushButton *close_;
	QPushButton *openPanel_;
	QTimer *timer_;
	QString shape_;
};

/* "Your stream moved to X": shown when a switch asked for from My OBS took the
 * stream away from this OBS. Not modal: OBS stays usable behind it. */
class HandoffNotice : public QDialog {
public:
	HandoffNotice(QWidget *parent, const QString &to, long long at_ms) : QDialog(parent)
	{
		setWindowTitle(QStringLiteral("EnhancedIRL Switcher"));
		setAttribute(Qt::WA_DeleteOnClose);
		setModal(false);
		setMinimumWidth(440);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(20, 18, 20, 16);
		layout->setSpacing(10);

		auto *title = text_label(this);
		title->setStyleSheet(QStringLiteral("QLabel { font-weight: bold; }"));
		title->setProperty("class", "text-large");
		title->setText(T("Handoff.Title").arg(to));
		auto *text = text_label(this);
		text->setText(T("Handoff.Text").arg(time_of_day(at_ms), to));

		auto *ok = new QPushButton(T("Button.GotIt"), this);
		auto *open = primary_button(this, T("Button.OpenPanel"));
		connect(ok, &QPushButton::clicked, this, &QDialog::close);
		connect(open, &QPushButton::clicked, this, [this] {
			eirl_agent_open_panel();
			close();
		});
		auto *buttons = new QHBoxLayout();
		buttons->addStretch();
		buttons->addWidget(ok);
		buttons->addWidget(open);

		layout->addWidget(title);
		layout->addWidget(text);
		layout->addSpacing(6);
		layout->addLayout(buttons);
		ok->setDefault(true);
	}
};

QPointer<SwitcherDialog> g_dialog;
QPointer<HandoffNotice> g_notice;

QWidget *main_window()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

struct HandoffTask {
	std::string to;
	long long at_ms;
};

void show_handoff_task(void *param)
{
	std::unique_ptr<HandoffTask> task(static_cast<HandoffTask *>(param));
	/* A newer notice replaces the one on screen. */
	if (g_notice)
		g_notice->close();
	const QString to = task->to.empty() ? QStringLiteral("OBS") : QString::fromStdString(task->to);
	g_notice = new HandoffNotice(main_window(), to, task->at_ms);
	g_notice->show();
	g_notice->raise();
}

} // namespace

extern "C" void eirl_ui_show_dialog(void)
{
	if (!g_dialog) {
		g_dialog = new SwitcherDialog(main_window());
		g_dialog->setAttribute(Qt::WA_DeleteOnClose);
	}
	g_dialog->show();
	g_dialog->raise();
	g_dialog->activateWindow();
}

extern "C" void eirl_ui_show_handoff(const char *to_name, long long at_ms)
{
	obs_queue_task(OBS_TASK_UI, show_handoff_task, new HandoffTask{to_name ? to_name : "", at_ms}, false);
}
