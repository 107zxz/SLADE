// -----------------------------------------------------------------------------
// SLADE - It's a Doom Editor
// Copyright(C) 2008 - 2026 Simon Judd
// -----------------------------------------------------------------------------

#include "Main.h"
#include "FighterEntryPanel.h"
#include "App.h"
#include "General/Misc.h"
#include "General/ResourceManager.h"
#include "General/UI.h"
#include "MainEditor/MainEditor.h"
#include "UI/Controls/ZoomControl.h"

using namespace slade;

FighterEntryPanel::FighterEntryPanel(wxWindow* parent) : EntryPanel(parent, "fighter"), playback_timer_{ this }
{
	canvas_ = new GfxCanvas(this, -1);
	canvas_->setViewType(GfxCanvas::View::Sprite);
	canvas_->allowScroll(true);
	canvas_->setPalette(maineditor::currentPalette());
	sizer_main_->Add(canvas_, 1, wxEXPAND);

	choice_state_ = new wxChoice(this, -1);
	button_prev_  = new wxButton(this, wxID_ANY, wxS("Previous"));
	button_next_  = new wxButton(this, wxID_ANY, wxS("Next"));
	button_play_  = new wxButton(this, wxID_ANY, wxS("Play"));
	label_frame_  = new wxStaticText(this, wxID_ANY, wxEmptyString);
	auto* zoom    = new ui::ZoomControl(this, canvas_);
	sizer_bottom_->Add(
		new wxStaticText(this, wxID_ANY, wxS("State:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	sizer_bottom_->Add(choice_state_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	sizer_bottom_->Add(button_prev_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	sizer_bottom_->Add(button_next_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	sizer_bottom_->Add(button_play_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	sizer_bottom_->Add(label_frame_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	sizer_bottom_->AddStretchSpacer();
	sizer_bottom_->Add(zoom, 0, wxEXPAND);

	choice_state_->Bind(wxEVT_CHOICE, &FighterEntryPanel::onStateChanged, this);
	button_prev_->Bind(wxEVT_BUTTON, &FighterEntryPanel::onPrevious, this);
	button_next_->Bind(wxEVT_BUTTON, &FighterEntryPanel::onNext, this);
	button_play_->Bind(wxEVT_BUTTON, &FighterEntryPanel::onPlay, this);
	Bind(wxEVT_TIMER, &FighterEntryPanel::onPlaybackTimer, this);
}

FighterEntryPanel::~FighterEntryPanel()
{
	playback_timer_.Stop();
}

bool FighterEntryPanel::loadEntry(ArchiveEntry* entry)
{
	if (!entry)
		return false;
	states_.clear();
	setPlaying(false);
	canvas_->image().clear();
	label_frame_->SetLabel(wxEmptyString);

	zscript::Definitions definitions;
	if (!definitions.parseZScript(entry) || definitions.classes().empty())
	{
		global::error = "No ZScript actor class found in fighter definition";
		return false;
	}

	for (const auto& [name, state] : definitions.classes().front().states().states())
	{
		DisplayState display_state;
		display_state.name = name;
		for (const auto& frame : state.frames)
			for (const auto sprite_frame : frame.sprite_frame)
				display_state.frames.push_back({ frame.sprite_base, sprite_frame, frame.duration });

		if (!display_state.frames.empty())
			states_.push_back(std::move(display_state));
	}
	auto first_state    = definitions.classes().front().states().firstState();
	auto first_state_it = std::find_if(
		states_.begin(), states_.end(), [&first_state](const auto& state) { return state.name == first_state; });
	if (first_state_it != states_.end())
		std::rotate(states_.begin(), first_state_it, first_state_it + 1);

	if (states_.empty())
	{
		global::error = "No sprite frames found in ZScript actor states";
		return false;
	}

	priority_archive_ = entry->parent();
	choice_state_->Clear();
	for (const auto& state : states_)
		choice_state_->Append(wxString::FromUTF8(state.name));
	choice_state_->SetSelection(0);
	current_frame_ = 0;
	updateFrame();
	return true;
}

void FighterEntryPanel::updateFrame()
{
	if (states_.empty() || choice_state_->GetSelection() < 0)
		return;

	const auto& state = states_[choice_state_->GetSelection()];
	if (state.frames.empty())
		return;
	current_frame_ %= state.frames.size();
	const auto& frame = state.frames[current_frame_];

	canvas_->image().clear();
	ArchiveEntry* sprite_entry = nullptr;
	int           image_index  = 0;
	const auto    sprite_name  = fmt::format("{}{}", frame.sprite_base, frame.sprite_frame);
	for (char rotation = '0'; rotation <= '8' && !sprite_entry; ++rotation)
		sprite_entry = app::resources().getPatchEntry(
			sprite_name + rotation, "sprites", priority_archive_, &image_index);

	if (sprite_entry && misc::loadImageFromEntry(&canvas_->image(), sprite_entry, image_index))
	{
		canvas_->setViewType(GfxCanvas::View::Sprite);
		canvas_->zoomToFit(true, 0.1);
		if (!playing_)
			canvas_->centerOnImageBounds();
		label_frame_->SetLabel(WX_FMT(
			"{} {} ({}/{}, duration {})",
			frame.sprite_base,
			frame.sprite_frame,
			static_cast<unsigned long>(current_frame_ + 1),
			static_cast<unsigned long>(state.frames.size()),
			frame.duration));
	}
	else
		label_frame_->SetLabel(WX_FMT(
			"{} {} ({}/{}): sprite not found",
			frame.sprite_base,
			frame.sprite_frame,
			static_cast<unsigned long>(current_frame_ + 1),
			static_cast<unsigned long>(state.frames.size())));

	canvas_->Refresh();
}

void FighterEntryPanel::onStateChanged(wxCommandEvent& e)
{
	current_frame_ = 0;
	if (playing_)
		setPlaying(true);
	updateFrame();
}

void FighterEntryPanel::onPrevious(wxCommandEvent& e)
{
	if (choice_state_->GetSelection() < 0 || states_.empty())
		return;
	const auto frame_count = states_[choice_state_->GetSelection()].frames.size();
	current_frame_         = (current_frame_ + frame_count - 1) % frame_count;
	updateFrame();
	if (playing_)
		setPlaying(true);
}

void FighterEntryPanel::onNext(wxCommandEvent& e)
{
	if (choice_state_->GetSelection() < 0 || states_.empty())
		return;
	current_frame_ = (current_frame_ + 1) % states_[choice_state_->GetSelection()].frames.size();
	updateFrame();
	if (playing_)
		setPlaying(true);
}

void FighterEntryPanel::setPlaying(bool playing)
{
	playing_ = playing && !states_.empty() && choice_state_->GetSelection() >= 0;
	playback_timer_.Stop();
	button_play_->SetLabel(playing_ ? wxS("Pause") : wxS("Play"));
	if (!playing_)
		return;

	const auto& state = states_[choice_state_->GetSelection()];
	if (state.frames.empty())
	{
		playing_ = false;
		button_play_->SetLabel(wxS("Play"));
		return;
	}

	const auto frame_duration = state.frames[current_frame_].duration;
	if (frame_duration < 0)
		return;

	const auto duration_ms = static_cast<int>((static_cast<int64_t>(frame_duration) * 1000 + 17) / 35);
	playback_timer_.StartOnce(std::max(1, duration_ms));
}

void FighterEntryPanel::onPlay(wxCommandEvent& e)
{
	setPlaying(!playing_);
}

void FighterEntryPanel::onPlaybackTimer(wxTimerEvent& e)
{
	if (!playing_ || states_.empty() || choice_state_->GetSelection() < 0)
		return;

	const auto frame_count = states_[choice_state_->GetSelection()].frames.size();
	if (frame_count == 0)
	{
		setPlaying(false);
		return;
	}

	current_frame_ = (current_frame_ + 1) % frame_count;
	updateFrame();
	setPlaying(true);
}