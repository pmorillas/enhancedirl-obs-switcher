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
 * The plugin's only window: link state, the pairing code, and a one-click way to
 * use the switcher's stream service. Everything else lives in the web panel.
 */

#include "switcher-dialog.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>

extern "C" {
#include "../agent/agent.hpp"
#include "../managed.h"
#include "../switcher-service.h"
}

namespace {

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

class SwitcherDialog : public QDialog {
public:
	explicit SwitcherDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle("EnhancedIRL Switcher");
		setMinimumWidth(420);

		status_ = new QLabel(this);
		status_->setWordWrap(true);
		code_ = new QLabel(this);
		code_->setAlignment(Qt::AlignCenter);
		code_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		QFont big = code_->font();
		big.setPointSize(big.pointSize() * 2);
		big.setBold(true);
		big.setFamily("monospace");
		code_->setFont(big);

		link_ = new QPushButton(T("Button.Link"), this);
		reopen_ = new QPushButton(T("Button.Reopen"), this);
		unlink_ = new QPushButton(T("Button.Unlink"), this);
		connect(link_, &QPushButton::clicked, [] { eirl_agent_begin_pairing(); });
		connect(reopen_, &QPushButton::clicked, [] { eirl_agent_reopen_browser(); });
		connect(unlink_, &QPushButton::clicked, [] { eirl_agent_unlink(); });

		service_ = new QLabel(this);
		service_->setWordWrap(true);
		useService_ = new QPushButton(T("Button.UseService"), this);
		connect(useService_, &QPushButton::clicked, [this] { use_switcher_service(); });

		auto *buttons = new QHBoxLayout();
		buttons->addWidget(link_);
		buttons->addWidget(reopen_);
		buttons->addWidget(unlink_);
		buttons->addStretch();

		auto *layout = new QVBoxLayout(this);
		layout->addWidget(status_);
		layout->addWidget(code_);
		layout->addLayout(buttons);
		layout->addSpacing(12);
		layout->addWidget(service_);
		layout->addWidget(useService_, 0, Qt::AlignLeft);

		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, [this] { refresh(); });
		timer_->start(500);
		refresh();
	}

private:
	void refresh()
	{
		eirl_agent_state s;
		eirl_agent_get_state(&s);

		QString text;
		if (EIRL_MANAGED_BUILD) {
			text = T(s.connected ? "Status.ManagedConnected" : "Status.ManagedConnecting");
		} else if (s.pairing) {
			text = s.code[0] ? T("Status.Confirm") : T("Status.Linking");
		} else if (s.linked) {
			QString who = s.account[0] ? QString::fromUtf8(s.account) : T("Status.YourAccount");
			text = T(s.connected ? "Status.LinkedConnected" : "Status.LinkedConnecting").arg(who);
		} else {
			text = T("Status.NotLinked");
		}
		if (s.error[0] && !s.linked)
			text += "\n\n" + error_text(s.error);
		status_->setText(text);

		code_->setVisible(s.pairing && s.code[0]);
		code_->setText(QString::fromUtf8(s.code));
		link_->setVisible(!EIRL_MANAGED_BUILD && !s.linked && !s.pairing);
		reopen_->setVisible(!EIRL_MANAGED_BUILD && s.pairing && s.code[0]);
		unlink_->setVisible(!EIRL_MANAGED_BUILD && s.linked);

		obs_service_t *service = obs_frontend_get_streaming_service();
		bool ours = service && switcher_service_platform(service) != nullptr;
		bool streaming = obs_frontend_streaming_active();
		service_->setText(ours ? T("Service.Ready") : T("Service.NotOurs"));
		/* A Cloud OBS moves to our service by itself (see plugin-main.c). */
		useService_->setVisible(!EIRL_MANAGED_BUILD && !ours);
		useService_->setEnabled(!streaming);
		useService_->setToolTip(streaming ? T("Service.StopFirst") : QString());
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

	QLabel *status_;
	QLabel *code_;
	QPushButton *link_;
	QPushButton *reopen_;
	QPushButton *unlink_;
	QLabel *service_;
	QPushButton *useService_;
	QTimer *timer_;
};

QPointer<SwitcherDialog> g_dialog;

} // namespace

extern "C" void eirl_ui_show_dialog(void)
{
	if (!g_dialog) {
		auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		g_dialog = new SwitcherDialog(main);
		g_dialog->setAttribute(Qt::WA_DeleteOnClose);
	}
	g_dialog->show();
	g_dialog->raise();
	g_dialog->activateWindow();
}
