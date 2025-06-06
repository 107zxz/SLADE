
// -----------------------------------------------------------------------------
// SLADE - It's a Doom Editor
// Copyright(C) 2008 - 2022 Simon Judd
//
// Email:       sirjuddington@gmail.com
// Web:         http://slade.mancubus.net
// Filename:    SLADEWxApp.cpp
// Description: SLADEWxApp class functions.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301, USA.
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
//
// Includes
//
// -----------------------------------------------------------------------------
#include "Main.h"
#include "SLADEWxApp.h"
#include "App.h"
#include "Archive/ArchiveManager.h"
#include "General/Console.h"
#include "MainEditor/MainEditor.h"
#include "MainEditor/UI/ArchiveManagerPanel.h"
#include "MainEditor/UI/MainWindow.h"
#include "MainEditor/UI/StartPage.h"
#include "OpenGL/OpenGL.h"
#include "UI/WxWebpHandler.h"
#include "Utility/Parser.h"
#include "Utility/StringUtils.h"
#include <wx/filefn.h>
#include <wx/statbmp.h>
#include <wx/url.h>
#include <wx/webrequest.h>
#undef BOOL
#ifdef UPDATEREVISION
#include "gitinfo.h"
#endif

using namespace slade;


// -----------------------------------------------------------------------------
//
// Variables
//
// -----------------------------------------------------------------------------
namespace slade::global
{
string error;

#ifdef GIT_DESCRIPTION
string sc_rev = GIT_DESCRIPTION;
#else
string sc_rev;
#endif

#ifdef DEBUG
bool debug = true;
#else
bool debug = false;
#endif

int win_version_major = 0;
int win_version_minor = 0;
} // namespace slade::global

string current_action;
bool   update_check_message_box = false;
CVAR(String, dir_last, "", CVar::Flag::Save)
CVAR(Bool, update_check, true, CVar::Flag::Save)
CVAR(Bool, update_check_beta, false, CVar::Flag::Save)


// -----------------------------------------------------------------------------
//
// Classes
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// SLADELog class
//
// Extension of the wxLog class to send all wxWidgets log messages
// to the SLADE log implementation
// -----------------------------------------------------------------------------
class SLADELog : public wxLog
{
protected:
	// wx2.9.x is no longer supported.
#if (wxMAJOR_VERSION < 3)
#error This will not compile with wxWidgets older than 3.0.0 !
#endif
	void DoLogText(const wxString& msg) override
	{
		static const string wx_prefix = "[wxWidgets] ";

		const auto msg_str = msg.utf8_string();
		if (msg.Lower().Contains(wxS("error")))
			log::error(wx_prefix + msg_str.substr(msg_str.size() - 10));
		else if (msg.Lower().Contains(wxS("warning")))
			log::warning(wx_prefix + msg_str.substr(msg_str.size() - 10));
		else
			log::info(wx_prefix + msg_str.substr(msg_str.size() - 10));
	}

public:
	SLADELog()           = default;
	~SLADELog() override = default;
};


// -----------------------------------------------------------------------------
// SLADEStackTrace class
//
// Extension of the wxStackWalker class that formats stack trace
// information to a multi-line string, that can be retrieved via
// getTraceString(). wxStackWalker is currently unimplemented on some
// platforms, so unfortunately it has to be disabled there
// -----------------------------------------------------------------------------
#if wxUSE_STACKWALKER
class SLADEStackTrace : public wxStackWalker
{
public:
	SLADEStackTrace() : stack_trace_("Stack Trace:\n") {}
	~SLADEStackTrace() override = default;

	const string& traceString() const { return stack_trace_; }
	const string& topLevel() const { return top_level_; }

	void OnStackFrame(const wxStackFrame& frame) override
	{
		string location = "[unknown location] ";
		if (frame.HasSourceLocation())
			location = fmt::format("({}:{}) ", frame.GetFileName().utf8_string(), frame.GetLine());

		wxUIntPtr address   = wxPtrToUInt(frame.GetAddress());
		string    func_name = frame.GetName().utf8_string();
		if (func_name.empty())
			func_name = fmt::format("[unknown:{}]", address);

		string line = fmt::format("{}{}", location, func_name);
		stack_trace_ += fmt::format("{}: {}\n", frame.GetLevel(), line);

		if (frame.GetLevel() == 0)
			top_level_ = line;
	}

private:
	string stack_trace_;
	string top_level_;
};


// -----------------------------------------------------------------------------
// SLADECrashDialog class
//
// A simple dialog that displays a crash message and a scrollable,
// multi-line textbox with a stack trace
// -----------------------------------------------------------------------------
class SLADECrashDialog : public wxDialog
{
public:
	SLADECrashDialog() : wxDialog(wxGetApp().GetTopWindow(), -1, wxS("SLADE Application Crash"))
	{
		// Setup sizer
		auto sizer = new wxBoxSizer(wxVERTICAL);
		SetSizer(sizer);

		auto hbox = new wxBoxSizer(wxHORIZONTAL);
		sizer->Add(hbox, 0, wxEXPAND);

		// Add dead doomguy picture
		app::archiveManager()
			.programResourceArchive()
			->entryAtPath("images/STFDEAD0.png")
			->exportFile(app::path("STFDEAD0.png", app::Dir::Temp));
		wxImage img;
		img.LoadFile(wxString::FromUTF8(app::path("STFDEAD0.png", app::Dir::Temp)));
		img.Rescale(img.GetWidth(), img.GetHeight(), wxIMAGE_QUALITY_NEAREST);
		auto picture = new wxStaticBitmap(this, -1, wxBitmap(img));
		hbox->Add(picture, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 10);

		// Add general crash message
		wxString message = wxS(
			"SLADE has crashed unexpectedly. To help fix the problem that caused this crash, "
			"please click 'Create GitHub Issue' below and complete the issue details on GitHub.");
		auto label = new wxStaticText(this, -1, message);
		hbox->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 10);
		label->Wrap(480 - 20 - picture->GetSize().x);

		// Add stack trace text area
		text_stack_ = new wxTextCtrl(
			this, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
		// text_stack_->SetValue(trace_);
		text_stack_->SetFont(wxFont(8, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
		sizer->Add(new wxStaticText(this, -1, wxS("Crash Information:")), 0, wxLEFT | wxRIGHT, 10);
		sizer->AddSpacer(2);
		sizer->Add(text_stack_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		// Add 'Copy Stack Trace' button
		hbox = new wxBoxSizer(wxHORIZONTAL);
		sizer->Add(hbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
		btn_copy_trace_ = new wxButton(this, -1, wxS("Copy Stack Trace"));
		hbox->AddStretchSpacer();
		hbox->Add(btn_copy_trace_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
		btn_copy_trace_->Bind(wxEVT_COMMAND_BUTTON_CLICKED, &SLADECrashDialog::onBtnCopyTrace, this);

		// Add 'Create GitHub Issue' button
		btn_send_ = new wxButton(this, -1, wxS("Create GitHub Issue"));
		hbox->Add(btn_send_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
		btn_send_->Bind(wxEVT_COMMAND_BUTTON_CLICKED, &SLADECrashDialog::onBtnPostReport, this);

		// Add 'Exit SLADE' button
		btn_exit_ = new wxButton(this, -1, wxS("Exit SLADE"));
		hbox->Add(btn_exit_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
		btn_exit_->Bind(wxEVT_COMMAND_BUTTON_CLICKED, &SLADECrashDialog::onBtnExit, this);

		// Setup layout
		wxDialog::Layout();
		SetInitialSize(wxSize(500, 600));
		CenterOnParent();
		Show(false);
	}

	~SLADECrashDialog() override = default;

	void loadStackTrace(const SLADEStackTrace& st)
	{
		top_level_ = st.topLevel();

		// SLADE info
		if (global::sc_rev.empty())
			trace_ = fmt::format("Version: {}", app::version().toString());
		else
			trace_ = fmt::format("Version: {} ({})", app::version().toString(), global::sc_rev);
		if (app::platform() == app::Platform::Windows)
			trace_ += fmt::format(" ({})\n", app::isWin64Build() ? "x64" : "x86");
		else
			trace_ += "\n";
		if (current_action.empty())
			trace_ += "No current action\n";
		else
			trace_ += fmt::format("Current action: {}", current_action);
		trace_ += "\n";

		// System info
		gl::Info gl_info = gl::sysInfo();
		trace_ += fmt::format("Operating System: {}\n", wxGetOsDescription().utf8_string());
		trace_ += fmt::format("Graphics Vendor: {}\n", gl_info.vendor);
		trace_ += fmt::format("Graphics Hardware: {}\n", gl_info.renderer);
		trace_ += fmt::format("OpenGL Version: {}\n", gl_info.version);

		// Stack trace
		trace_ += "\n";
		trace_ += st.traceString();

		// Last 10 log lines
		trace_ += "\nLast Log Messages:\n";
		auto& log = log::history();
		for (auto a = log.size() - 10; a < log.size(); a++)
			trace_ += log[a].message + "\n";

		// Set stack trace text
		text_stack_->SetValue(wxString::FromUTF8(trace_));

		// Dump stack trace to a file (just in case)
		wxFile file(wxString::FromUTF8(app::path("slade3_crash.log", app::Dir::User)), wxFile::write);
		file.Write(wxString::FromUTF8(trace_));
		file.Close();

		// Also dump stack trace to console
		std::cerr << trace_;
	}

	void onBtnCopyTrace(wxCommandEvent& e)
	{
		if (wxTheClipboard->Open())
		{
			wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(trace_)));
			wxTheClipboard->Flush();
			wxTheClipboard->Close();
			wxMessageBox(wxS("Stack trace successfully copied to clipboard"));
		}
		else
			wxMessageBox(
				wxS("Unable to access the system clipboard, please select+copy the text above manually"),
				wxS("Error"),
				wxICON_EXCLAMATION);
	}

	void onBtnPostReport(wxCommandEvent& e)
	{
		auto url_base = "https://github.com/sirjuddington/SLADE/issues/new?labels=crash+bug&template=crash.yml";
		auto version  = global::sc_rev.empty() ? app::version().toString()
											   : app::version().toString() + " " + global::sc_rev;

		wxURL url(wxString::FromUTF8(fmt::format("{}&version={}&crashinfo={}", url_base, version, trace_)));

		wxLaunchDefaultBrowser(url.BuildURI());
	}

	void onBtnExit(wxCommandEvent& e) { EndModal(wxID_OK); }

private:
	wxTextCtrl* text_stack_;
	wxButton*   btn_copy_trace_;
	wxButton*   btn_exit_;
	wxButton*   btn_send_;
	string      trace_;
	string      top_level_;
};
#endif // wxUSE_STACKWALKER


// -----------------------------------------------------------------------------
// MainAppFileListener and related Classes
//
// wxWidgets IPC classes used to send filenames of archives to open
// from one SLADE instance to another in the case where a second
// instance is opened
// -----------------------------------------------------------------------------
class MainAppFLConnection : public wxConnection
{
public:
	MainAppFLConnection()           = default;
	~MainAppFLConnection() override = default;

	bool OnAdvise(const wxString& topic, const wxString& item, const void* data, size_t size, wxIPCFormat format)
		override
	{
		return true;
	}

	bool OnPoke(const wxString& topic, const wxString& item, const void* data, size_t size, wxIPCFormat format) override
	{
		app::archiveManager().openArchive(item.utf8_string());
		return true;
	}
};

class MainAppFileListener : public wxServer
{
public:
	MainAppFileListener()           = default;
	~MainAppFileListener() override = default;

	wxConnectionBase* OnAcceptConnection(const wxString& topic) override { return new MainAppFLConnection(); }
};

class MainAppFLClient : public wxClient
{
public:
	MainAppFLClient()           = default;
	~MainAppFLClient() override = default;

	wxConnectionBase* OnMakeConnection() override { return new MainAppFLConnection(); }
};


// -----------------------------------------------------------------------------
//
// SLADEWxApp Class Functions
//
// -----------------------------------------------------------------------------
IMPLEMENT_APP(SLADEWxApp)


// -----------------------------------------------------------------------------
// Checks if another instance of SLADE is already running, and if so, sends the
// args to the file listener of the existing SLADE process.
// Returns false if another instance was found and the new SLADE was started
// with arguments
// -----------------------------------------------------------------------------
bool SLADEWxApp::singleInstanceCheck()
{
	auto data_dir = wxStandardPaths::Get().GetUserDataDir();
	if (!wxDirExists(data_dir))
		wxMkdir(data_dir);

	single_instance_checker_ = new wxSingleInstanceChecker;
	single_instance_checker_->Create(WX_FMT("SLADE-{}", app::version().toString()), data_dir);

	if (argc == 1)
		return true;

	if (single_instance_checker_->IsAnotherRunning())
	{
		delete single_instance_checker_;

		// Connect to the file listener of the existing SLADE process
		auto client     = std::make_unique<MainAppFLClient>();
		auto connection = client->MakeConnection(wxGetHostName(), wxS("SLADE_MAFL"), wxS("files"));

		if (connection)
		{
			// Send args as archives to open
			for (int a = 1; a < argc; a++)
				connection->Poke(argv[a], argv[a]);

			connection->Disconnect();
		}

		return false;
	}

	return true;
}

// -----------------------------------------------------------------------------
// Application initialization, run when program is started
// -----------------------------------------------------------------------------
bool SLADEWxApp::OnInit()
{
	// Check if an instance of SLADE is already running
	if (!singleInstanceCheck())
	{
		printf("Found active instance. Quitting.\n");
		return false;
	}

	// Init wxSocket stuff (for WebGet)
	wxSocketBase::Initialize();

	// Start up file listener
	file_listener_ = new MainAppFileListener();
	file_listener_->Create(wxS("SLADE_MAFL"));

	// Setup system options
	wxSystemOptions::SetOption(wxS("mac.listctrl.always_use_generic"), 1);

	// Set application name (for wx directory stuff)
#ifdef __WINDOWS__
	wxApp::SetAppName(wxS("SLADE3"));
#else
	wxApp::SetAppName(wxS("slade3"));
#endif

	// Handle exceptions using wxDebug stuff, but only in release mode
#ifdef NDEBUG
	wxHandleFatalExceptions(true);
#endif

	// Load image handlers
	wxInitAllImageHandlers();
	wxImage::AddHandler(new WxWebpHandler);

#ifdef __APPLE__
	// Should be constant, wxWidgets Cocoa backend scales everything under the hood
	const double ui_scale = 1.0;
#else  // !__APPLE__
	// Calculate scaling factor (from system ppi)
	wxMemoryDC dc;
	double     ui_scale = (double)(dc.GetPPI().x) / 96.0;
	if (ui_scale < 1.)
		ui_scale = 1.;
#endif // __APPLE__

	// Get Windows version
#ifdef __WXMSW__
	wxGetOsVersion(&global::win_version_major, &global::win_version_minor);
	log::info("Windows Version: {}.{}", global::win_version_major, global::win_version_minor);
#endif

	// Reroute wx log messages
	wxLog::SetActiveTarget(new SLADELog());

	// Get command line arguments
	vector<string> args;
	for (int a = 1; a < argc; a++)
		args.push_back(argv[a].utf8_string());

	// Init application
	try
	{
		if (!app::init(args, 1.))
			return false;
	}
	catch (const std::exception& ex)
	{
		log::error("Exception during SLADE initialization: {}", ex.what());
		throw;
	}

	// Init crash dialog
	// Do it now rather than after a crash happens, since it may fail depending on the type of crash
#if wxUSE_STACKWALKER
#ifndef _DEBUG
	crash_dialog_ = new SLADECrashDialog();
#endif //_DEBUG
#endif // wxUSE_STACKWALKER

	// Check for updates
#ifdef __WXMSW__
	wxHTTP::Initialize();
	if (update_check)
		checkForUpdates(false);
#endif

	// Bind events
	Bind(wxEVT_MENU, &SLADEWxApp::onMenu, this);
	Bind(wxEVT_WEBREQUEST_STATE, &SLADEWxApp::onVersionCheckCompleted, this);
	Bind(wxEVT_ACTIVATE_APP, &SLADEWxApp::onActivate, this);
	Bind(wxEVT_QUERY_END_SESSION, &SLADEWxApp::onEndSession, this);

	return true;
}

// -----------------------------------------------------------------------------
// Application shutdown, run when program is closed
// -----------------------------------------------------------------------------
int SLADEWxApp::OnExit()
{
	wxSocketBase::Shutdown();
	delete single_instance_checker_;
	delete file_listener_;

	return 0;
}

// -----------------------------------------------------------------------------
// Handler for when a fatal exception occurs - show the stack trace/crash dialog
// if it's configured to be used
// -----------------------------------------------------------------------------
void SLADEWxApp::OnFatalException()
{
#if wxUSE_STACKWALKER
#ifndef _DEBUG
	SLADEStackTrace st;
	st.WalkFromException();
	crash_dialog_->loadStackTrace(st);
	crash_dialog_->ShowModal();
#endif //_DEBUG
#endif // wxUSE_STACKWALKER
}

// -----------------------------------------------------------------------------
// Handler for when an unhandled exception occurs - log it to the console
// -----------------------------------------------------------------------------
bool SLADEWxApp::OnExceptionInMainLoop()
{
	try
	{
		throw;
	}
	catch (const std::exception& ex)
	{
		log::error("Unhandled exception: {}", ex.what());
	}

	return wxApp::OnExceptionInMainLoop();
}

#ifdef __APPLE__
void SLADEWxApp::MacOpenFile(const wxString& fileName)
{
	theMainWindow->archiveManagerPanel()->openFile(fileName.utf8_string());
}
#endif // __APPLE__

// -----------------------------------------------------------------------------
// Runs the version checker, if [message_box] is true, a message box will be
// shown if already up-to-date
// -----------------------------------------------------------------------------
void SLADEWxApp::checkForUpdates(bool message_box)
{
#ifdef __WXMSW__
	update_check_message_box = message_box;
	log::info(1, "Checking for updates...");
	auto request = wxWebSession::GetDefault().CreateRequest(this, wxS("https://slade.mancubus.net/version_win.txt"));
	request.Start();
#endif
}


// -----------------------------------------------------------------------------
//
// SLADEWxApp Class Events
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Called when a menu item is selected in the application
// -----------------------------------------------------------------------------
void SLADEWxApp::onMenu(wxCommandEvent& e)
{
	bool handled = false;

	// Find applicable action
	auto s_action = SAction::fromWxId(e.GetId());
	auto action   = s_action->id();

	// Handle action if valid
	if (action != "invalid")
	{
		current_action = action;
		SActionHandler::setWxIdOffset(e.GetId() - s_action->wxId());
		handled = SActionHandler::doAction(action);

		// Check if triggering object is a menu item
		if (s_action && s_action->type() == SAction::Type::Check)
		{
			if (e.GetEventObject() && e.GetEventObject()->IsKindOf(wxCLASSINFO(wxMenuItem)))
			{
				auto item = (wxMenuItem*)e.GetEventObject();
				item->Check(s_action->isChecked());
			}
		}

		current_action = "";
	}

	// If not handled, let something else handle it
	if (!handled)
		e.Skip();
}

// -----------------------------------------------------------------------------
// Called when the version check thread completes
// -----------------------------------------------------------------------------
void SLADEWxApp::onVersionCheckCompleted(wxWebRequestEvent& e)
{
	// Check failed
	if (e.GetState() == wxWebRequest::State_Failed || e.GetState() == wxWebRequest::State_Unauthorized)
	{
		log::error("Version check failed, unable to connect");
		if (update_check_message_box)
			wxMessageBox(
				wxS("Update check failed: unable to connect to internet. "
					"Check your connection and try again."),
				wxS("Check for Updates"));

		return;
	}

	// If not completed, ignore
	if (e.GetState() != wxWebRequest::State_Completed)
		return;

	// Parse version info
	app::Version stable, beta;
	string       bin_stable, installer_stable, bin_beta; // Currently unused but may be useful in the future
	Parser       parser;
	auto         response_string = e.GetResponse().AsString();
	if (parser.parseText(response_string.utf8_string()))
	{
		// Stable
		auto node_stable = parser.parseTreeRoot()->childPTN("stable");
		if (node_stable)
		{
			// Version
			auto node_version = node_stable->childPTN("version");
			if (node_version)
			{
				stable.major    = node_version->intValue(0);
				stable.minor    = node_version->intValue(1);
				stable.revision = node_version->intValue(2);
			}

			// Binaries link
			auto node_bin = node_stable->childPTN("bin");
			if (node_bin)
				bin_stable = node_bin->stringValue();

			// Installer link
			auto node_install = node_stable->childPTN("install");
			if (node_install)
				installer_stable = node_install->stringValue();
		}

		// Beta
		auto node_beta = parser.parseTreeRoot()->childPTN("beta");
		if (node_beta)
		{
			// Version
			auto node_version = node_beta->childPTN("version");
			if (node_version)
			{
				beta.major    = node_version->intValue(0);
				beta.minor    = node_version->intValue(1);
				beta.revision = node_version->intValue(2);
			}

			// Beta number
			auto node_beta_num = node_beta->childPTN("beta");
			if (node_beta_num)
				beta.beta = node_beta_num->intValue();

			// Binaries link
			auto node_bin = node_beta->childPTN("bin");
			if (node_bin)
				bin_beta = node_bin->stringValue();
		}
	}

	// Check for correct info
	if (stable.major == 0 || beta.major == 0)
	{
		log::warning("Version check failed, received invalid version info");
		log::debug("Received version text:\n\n{}", response_string.utf8_string());
		if (update_check_message_box)
			wxMessageBox(wxS("Update check failed: received invalid version info."), wxS("Check for Updates"));
		return;
	}

	log::info("Latest stable release: v{}", stable.toString());
	log::info("Latest beta release: v{}", beta.toString());

	// Check if new stable version
	bool new_stable = app::version().cmp(stable) < 0;
	bool new_beta   = app::version().cmp(beta) < 0;

	// Set up for new beta/stable version prompt (if any)
	string message, caption, version;
	if (update_check_beta && new_beta)
	{
		// New Beta
		caption = "New Beta Version Available";
		version = beta.toString();
		message = fmt::format(
			"A new beta version of SLADE is available ({}), click OK to visit the SLADE homepage "
			"and download the update.",
			version);
	}
	else if (new_stable)
	{
		// New Stable
		caption = "New Version Available";
		version = stable.toString();
		message = fmt::format(
			"A new version of SLADE is available ({}), click OK to visit the SLADE homepage and "
			"download the update.",
			version);
	}
	else
	{
		// No update
		log::info(1, "Already up-to-date");
		if (update_check_message_box)
			wxMessageBox(wxS("SLADE is already up to date"), wxS("Check for Updates"));

		return;
	}

	// Prompt to update
	auto main_window = maineditor::window();
	if (main_window->startPageTabOpen() && app::useWebView())
	{
		// Start Page (webview version) is open, show it there
		main_window->openStartPageTab();
		main_window->startPage()->updateAvailable(version);
	}
	else
	{
		// No start page, show a message box
		if (wxMessageBox(wxString::FromUTF8(message), wxString::FromUTF8(caption), wxOK | wxCANCEL) == wxOK)
			wxLaunchDefaultBrowser(wxS("http://slade.mancubus.net/index.php?page=downloads"));
	}
}

// -----------------------------------------------------------------------------
// Called when the app gains focus
// -----------------------------------------------------------------------------
void SLADEWxApp::onActivate(wxActivateEvent& e)
{
	if (!e.GetActive() || app::isExiting())
	{
		e.Skip();
		return;
	}

	// Check open directory archives for changes on the file system
	if (theMainWindow && theMainWindow->archiveManagerPanel())
		theMainWindow->archiveManagerPanel()->checkDirArchives();

	e.Skip();
}

// -----------------------------------------------------------------------------
// Called when the system is ending the session (shutdown/restart)
// -----------------------------------------------------------------------------
void SLADEWxApp::onEndSession(wxCloseEvent& e)
{
	session_ending_ = true;
	maineditor::windowWx()->Close();
	e.Skip();
}


// -----------------------------------------------------------------------------
//
// Console Commands
//
// -----------------------------------------------------------------------------

CONSOLE_COMMAND(crash, 0, false)
{
	if (wxMessageBox(
			wxS("Yes, this command does actually exist and *will* crash the program. Do you really want it to crash?"),
			wxS("...Really?"),
			wxYES_NO | wxCENTRE)
		== wxYES)
	{
		uint8_t* test = nullptr;
		*test         = 5;
	}
}

CONSOLE_COMMAND(quit, 0, true)
{
	bool save_config = true;
	for (auto& arg : args)
	{
		if (strutil::equalCI(arg, "nosave"))
			save_config = false;
	}

	app::exit(save_config);
}
