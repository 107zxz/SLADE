// -----------------------------------------------------------------------------
// SLADE - It's a Doom Editor
// Copyright(C) 2008 - 2026 Simon Judd
// -----------------------------------------------------------------------------

#include "Main.h"
#include "FighterEntryPanel.h"
#include "App.h"
#include "Game/Game.h"
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

	choice_state_         = new wxChoice(this, -1);
	button_prev_          = new wxButton(this, wxID_ANY, wxS("Previous"));
	button_next_          = new wxButton(this, wxID_ANY, wxS("Next"));
	button_play_          = new wxButton(this, wxID_ANY, wxS("Play"));
	label_frame_          = new wxStaticText(this, wxID_ANY, wxEmptyString);
	choice_hit_line_      = new wxChoice(this, -1);
	button_add_hit_line_  = new wxButton(this, wxID_ANY, wxS("Add"));
	spin_hit_line_length_ = new wxSpinCtrlDouble(
		this,
		-1,
		wxEmptyString,
		wxDefaultPosition,
		wxDefaultSize,
		wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
		-100000,
		100000,
		0,
		1);
	spin_hit_line_x_ = new wxSpinCtrlDouble(
		this,
		-1,
		wxEmptyString,
		wxDefaultPosition,
		wxDefaultSize,
		wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
		-100000,
		100000,
		0,
		1);
	spin_hit_line_y_ = new wxSpinCtrlDouble(
		this,
		-1,
		wxEmptyString,
		wxDefaultPosition,
		wxDefaultSize,
		wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
		-100000,
		100000,
		0,
		1);
	spin_hit_line_length_->SetDigits(3);
	spin_hit_line_x_->SetDigits(3);
	spin_hit_line_y_->SetDigits(3);
	spin_hit_line_length_->SetIncrement(0.25);
	spin_hit_line_x_->SetIncrement(0.25);
	spin_hit_line_y_->SetIncrement(0.25);
	label_hit_line_         = new wxStaticText(this, wxID_ANY, wxS("No HitLines"));
	auto* zoom              = new ui::ZoomControl(this, canvas_);
	auto* bottom_layout     = new wxBoxSizer(wxVERTICAL);
	auto* controls          = new wxBoxSizer(wxHORIZONTAL);
	auto* hit_line_controls = new wxBoxSizer(wxHORIZONTAL);
	controls->Add(new wxStaticText(this, wxID_ANY, wxS("State:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->Add(choice_state_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->Add(button_prev_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->Add(button_next_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->Add(button_play_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->Add(label_frame_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->Add(new wxStaticText(this, wxID_ANY, wxS("HitLine:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->Add(choice_hit_line_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->Add(button_add_hit_line_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	controls->AddStretchSpacer();
	controls->Add(zoom, 0, wxEXPAND);
	bottom_layout->Add(controls, 0, wxEXPAND);
	hit_line_controls->Add(
		new wxStaticText(this, wxID_ANY, wxS("Length:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	hit_line_controls->Add(spin_hit_line_length_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	hit_line_controls->Add(
		new wxStaticText(this, wxID_ANY, wxS("X:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	hit_line_controls->Add(spin_hit_line_x_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	hit_line_controls->Add(
		new wxStaticText(this, wxID_ANY, wxS("Y:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	hit_line_controls->Add(spin_hit_line_y_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	hit_line_controls->Add(label_hit_line_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
	bottom_layout->Add(hit_line_controls, 0, wxEXPAND);
	sizer_hit_line_parameters_ = new wxFlexGridSizer(4, ui::pad(), ui::pad());
	bottom_layout->Add(sizer_hit_line_parameters_, 0, wxEXPAND | wxTOP, ui::pad());
	sizer_bottom_->Add(bottom_layout, 1, wxEXPAND);

	choice_state_->Bind(wxEVT_CHOICE, &FighterEntryPanel::onStateChanged, this);
	button_prev_->Bind(wxEVT_BUTTON, &FighterEntryPanel::onPrevious, this);
	button_next_->Bind(wxEVT_BUTTON, &FighterEntryPanel::onNext, this);
	button_play_->Bind(wxEVT_BUTTON, &FighterEntryPanel::onPlay, this);
	choice_hit_line_->Bind(wxEVT_CHOICE, &FighterEntryPanel::onHitLineChanged, this);
	button_add_hit_line_->Bind(wxEVT_BUTTON, &FighterEntryPanel::onAddHitLine, this);
	spin_hit_line_length_->Bind(wxEVT_SPINCTRLDOUBLE, &FighterEntryPanel::onHitLineValueChanged, this);
	spin_hit_line_x_->Bind(wxEVT_SPINCTRLDOUBLE, &FighterEntryPanel::onHitLineValueChanged, this);
	spin_hit_line_y_->Bind(wxEVT_SPINCTRLDOUBLE, &FighterEntryPanel::onHitLineValueChanged, this);
	spin_hit_line_length_->Bind(wxEVT_TEXT_ENTER, &FighterEntryPanel::onHitLineValueChanged, this);
	spin_hit_line_x_->Bind(wxEVT_TEXT_ENTER, &FighterEntryPanel::onHitLineValueChanged, this);
	spin_hit_line_y_->Bind(wxEVT_TEXT_ENTER, &FighterEntryPanel::onHitLineValueChanged, this);
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
	source_entry_ = entry;
	source_text_.assign(reinterpret_cast<const char*>(entry->rawData()), entry->size());
	setPlaying(false);
	canvas_->image().clear();
	label_frame_->SetLabel(wxEmptyString);
	choice_hit_line_->Clear();
	choice_hit_line_->Enable(false);
	label_hit_line_->SetLabel(wxS("No HitLines"));

	zscript::Definitions definitions;
	if (!definitions.parseZScript(entry) || definitions.classes().empty())
	{
		states_.clear();
		global::error = "No ZScript actor class found in fighter definition";
		return false;
	}

	for (const auto& [name, state] : definitions.classes().front().states().states())
	{
		DisplayState display_state;
		display_state.name = name;
		for (const auto& frame : state.frames)
			for (const auto sprite_frame : frame.sprite_frame)
			{
				DisplayFrame display_frame{ frame.sprite_base, sprite_frame, frame.duration };
				display_frame.hit_line_insert_pos = frame.hit_line_insert_pos;
				for (const auto& hit_line : frame.hit_lines)
					display_frame.hit_lines.push_back(hit_line);
				display_state.frames.push_back(std::move(display_frame));
			}

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
	actor_scale_         = { 0.2, 0.2 };
	actor_height_        = 53;
	const auto& defaults = definitions.classes().front().defaultProperties();
	if (const auto scale = defaults.getIf<string>("scale"))
	{
		strutil::toDouble(*scale, actor_scale_.x);
		actor_scale_.y = actor_scale_.x;
	}
	if (const auto scale_x = defaults.getIf<string>("scalex"))
		strutil::toDouble(*scale_x, actor_scale_.x);
	if (const auto scale_y = defaults.getIf<string>("scaley"))
		strutil::toDouble(*scale_y, actor_scale_.y);
	if (const auto height = defaults.getIf<string>("height"))
		strutil::toDouble(*height, actor_height_);
	if (actor_scale_.x == 0)
		actor_scale_.x = 0.2;
	if (actor_scale_.y == 0)
		actor_scale_.y = 0.2;

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
	current_hit_line_ = 0;
	choice_hit_line_->Clear();
	for (size_t i = 0; i < frame.hit_lines.size(); ++i)
		choice_hit_line_->Append(WX_FMT("{}", static_cast<unsigned long>(i + 1)));
	choice_hit_line_->Enable(!frame.hit_lines.empty());
	button_add_hit_line_->Enable(source_entry_ != nullptr);
	if (!frame.hit_lines.empty())
		choice_hit_line_->SetSelection(0);
	spin_hit_line_length_->Enable(!frame.hit_lines.empty());
	spin_hit_line_x_->Enable(!frame.hit_lines.empty());
	spin_hit_line_y_->Enable(!frame.hit_lines.empty());
	updateHitLineInfo();

	canvas_->setHitLineOverlays({});
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
		canvas_->setActorSpriteScale(actor_scale_.x, actor_scale_.y);
		updateHitLineOverlays();
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

void FighterEntryPanel::updateHitLineInfo()
{
	updating_hit_line_controls_ = true;
	sizer_hit_line_parameters_->Clear(true);
	text_hit_line_parameters_.clear();

	const zscript::State::HitLine* selected_hit_line = nullptr;
	size_t                         selected_index    = 0;
	if (!states_.empty() && choice_state_->GetSelection() >= 0)
	{
		const auto& state = states_[choice_state_->GetSelection()];
		if (current_frame_ < state.frames.size())
		{
			const auto& hit_lines = state.frames[current_frame_].hit_lines;
			const auto  selection = choice_hit_line_->GetSelection();
			if (selection >= 0 && static_cast<size_t>(selection) < hit_lines.size())
			{
				selected_index    = static_cast<size_t>(selection);
				selected_hit_line = &hit_lines[selected_index];
				current_hit_line_ = selected_index;
			}
		}
	}

	const auto text_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	const auto colour      = selected_hit_line ? GfxCanvas::hitLineColour(selected_index).toWx() : text_colour;
	choice_hit_line_->SetForegroundColour(colour);
	label_hit_line_->SetForegroundColour(colour);
	if (selected_hit_line)
	{
		const bool can_edit = selected_hit_line->source && selected_hit_line->source == source_entry_;
		spin_hit_line_length_->SetValue(selected_hit_line->length);
		spin_hit_line_x_->SetValue(selected_hit_line->x);
		spin_hit_line_y_->SetValue(selected_hit_line->y);
		spin_hit_line_length_->Enable(can_edit);
		spin_hit_line_x_->Enable(can_edit);
		spin_hit_line_y_->Enable(can_edit);
		label_hit_line_->SetLabel(can_edit ? wxEmptyString : wxS("Defined in included source (read-only)"));
	}
	else
	{
		label_hit_line_->SetLabel(wxS("No HitLines"));
		spin_hit_line_length_->SetValue(0);
		spin_hit_line_x_->SetValue(0);
		spin_hit_line_y_->SetValue(0);
		spin_hit_line_length_->Enable(false);
		spin_hit_line_x_->Enable(false);
		spin_hit_line_y_->Enable(false);
	}

	size_t max_parameters = 0;
	for (const auto& state : states_)
		for (const auto& frame : state.frames)
			for (const auto& hit_line : frame.hit_lines)
				max_parameters = std::max(max_parameters, hit_line.additional_parameters.size());

	const bool can_edit = selected_hit_line && selected_hit_line->source == source_entry_;
	static constexpr std::array<const char*, 5> parameter_names = {
		"Hitstun", "Hit FX", "Knockback", "Move ID", "Flags"
	};
	for (size_t i = 0; i < max_parameters; ++i)
	{
		const bool has_parameter = selected_hit_line && i < selected_hit_line->additional_parameters.size();
		const auto label_text    = i < parameter_names.size() ? wxString::FromUTF8(parameter_names[i])
															  : WX_FMT("Arg {}", static_cast<unsigned long>(i + 3));
		auto*      label         = new wxStaticText(this, wxID_ANY, label_text + wxS(":"));
		label->Enable(has_parameter);
		auto* text = new wxTextCtrl(
			this,
			wxID_ANY,
			has_parameter ? wxString::FromUTF8(selected_hit_line->additional_parameters[i].value) : wxString{},
			wxDefaultPosition,
			wxSize(360, -1),
			wxTE_PROCESS_ENTER);
		text->Enable(can_edit && has_parameter);
		text->Bind(wxEVT_TEXT, &FighterEntryPanel::onHitLineParameterChanged, this);
		sizer_hit_line_parameters_->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
		sizer_hit_line_parameters_->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, ui::pad());
		text_hit_line_parameters_.push_back(text);
	}
	Layout();
	updating_hit_line_controls_ = false;
}

void FighterEntryPanel::updateHitLineOverlays()
{
	if (states_.empty() || choice_state_->GetSelection() < 0)
		return;

	const auto& state = states_[choice_state_->GetSelection()];
	if (current_frame_ >= state.frames.size())
		return;

	vector<GfxCanvas::HitLine> hit_lines;
	for (const auto& hit_line : state.frames[current_frame_].hit_lines)
		hit_lines.push_back({ hit_line.length, hit_line.x, hit_line.y + actor_height_ });
	canvas_->setHitLineOverlays(hit_lines);
}

void FighterEntryPanel::onHitLineValueChanged(wxCommandEvent& e)
{
	if (updating_hit_line_controls_ || states_.empty() || choice_state_->GetSelection() < 0)
		return;

	auto& state = states_[choice_state_->GetSelection()];
	if (current_frame_ >= state.frames.size())
		return;
	auto& hit_lines = state.frames[current_frame_].hit_lines;
	if (current_hit_line_ >= hit_lines.size())
		return;

	auto&      hit_line = hit_lines[current_hit_line_];
	const auto length   = spin_hit_line_length_->GetValue();
	const auto x        = spin_hit_line_x_->GetValue();
	const auto y        = spin_hit_line_y_->GetValue();
	if (!hit_line.added)
		for (auto& sibling_state : states_)
			for (auto& sibling_frame : sibling_state.frames)
				for (auto& sibling_line : sibling_frame.hit_lines)
					if (!sibling_line.added && sibling_line.source == hit_line.source
						&& sibling_line.length_range.start == hit_line.length_range.start
						&& sibling_line.x_range.start == hit_line.x_range.start
						&& sibling_line.y_range.start == hit_line.y_range.start)
					{
						sibling_line.length = length;
						sibling_line.x      = x;
						sibling_line.y      = y;
					}
	setModified();
	updateHitLineOverlays();
	updateHitLineInfo();
}

void FighterEntryPanel::onHitLineParameterChanged(wxCommandEvent& e)
{
	if (updating_hit_line_controls_ || states_.empty() || choice_state_->GetSelection() < 0)
		return;

	auto*      text    = static_cast<wxTextCtrl*>(e.GetEventObject());
	const auto control = std::find(text_hit_line_parameters_.begin(), text_hit_line_parameters_.end(), text);
	if (control == text_hit_line_parameters_.end())
		return;
	const auto parameter_index = static_cast<size_t>(std::distance(text_hit_line_parameters_.begin(), control));
	auto&      state           = states_[choice_state_->GetSelection()];
	if (current_frame_ >= state.frames.size())
		return;
	auto& hit_lines = state.frames[current_frame_].hit_lines;
	if (current_hit_line_ >= hit_lines.size())
		return;

	auto& hit_line = hit_lines[current_hit_line_];
	if (parameter_index >= hit_line.additional_parameters.size())
		return;
	const auto value = text->GetValue().ToStdString();
	for (auto& sibling_state : states_)
		for (auto& sibling_frame : sibling_state.frames)
			for (auto& sibling_line : sibling_frame.hit_lines)
				if (sibling_line.source == hit_line.source
					&& sibling_line.length_range.start == hit_line.length_range.start
					&& parameter_index < sibling_line.additional_parameters.size()
					&& sibling_line.additional_parameters[parameter_index].range.start
						   == hit_line.additional_parameters[parameter_index].range.start)
					sibling_line.additional_parameters[parameter_index].value = value;
	setModified();
}

void FighterEntryPanel::onStateChanged(wxCommandEvent& e)
{
	current_frame_ = 0;
	if (playing_)
		setPlaying(true);
	updateFrame();
}

void FighterEntryPanel::onHitLineChanged(wxCommandEvent& e)
{
	updateHitLineInfo();
}

void FighterEntryPanel::onAddHitLine(wxCommandEvent& e)
{
	if (states_.empty() || choice_state_->GetSelection() < 0)
		return;

	auto& state = states_[choice_state_->GetSelection()];
	if (current_frame_ >= state.frames.size())
		return;

	zscript::State::HitLine hit_line;
	hit_line.source = source_entry_;
	hit_line.added  = true;
	auto& hit_lines = state.frames[current_frame_].hit_lines;
	hit_lines.push_back(std::move(hit_line));
	updateFrame();
	choice_hit_line_->SetSelection(static_cast<int>(hit_lines.size() - 1));
	updateHitLineInfo();
	setModified();
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

bool FighterEntryPanel::writeEntry(ArchiveEntry& entry)
{
	if (&entry != source_entry_ || source_text_.empty())
		return false;

	struct Replacement
	{
		unsigned start;
		unsigned end;
		string   value;
	};
	vector<Replacement>                 replacements;
	vector<std::pair<unsigned, string>> added_hit_lines;
	for (const auto& state : states_)
		for (const auto& frame : state.frames)
			for (const auto& hit_line : frame.hit_lines)
			{
				if (hit_line.added)
				{
					const auto call = fmt::format(
						" A_HitLine({:.6g}, ({:.6g}, {:.6g}))", hit_line.length, hit_line.x, hit_line.y);
					auto insertion = std::find_if(
						added_hit_lines.begin(),
						added_hit_lines.end(),
						[&frame](const auto& item) { return item.first == frame.hit_line_insert_pos; });
					if (insertion == added_hit_lines.end())
						added_hit_lines.emplace_back(frame.hit_line_insert_pos, call);
					else
						insertion->second += call;
					continue;
				}
				if (hit_line.source != source_entry_)
					continue;
				if (hit_line.length != hit_line.original_length)
					replacements.push_back(
						{ hit_line.length_range.start,
						  hit_line.length_range.end,
						  fmt::format("{:.6g}", hit_line.length) });
				if (hit_line.x != hit_line.original_x)
					replacements.push_back(
						{ hit_line.x_range.start, hit_line.x_range.end, fmt::format("{:.6g}", hit_line.x) });
				if (hit_line.y != hit_line.original_y)
					replacements.push_back(
						{ hit_line.y_range.start, hit_line.y_range.end, fmt::format("{:.6g}", hit_line.y) });
				for (const auto& parameter : hit_line.additional_parameters)
					if (parameter.value != parameter.original_value)
						replacements.push_back({ parameter.range.start, parameter.range.end, parameter.value });
			}
	for (const auto& [position, calls] : added_hit_lines)
		replacements.push_back({ position, position, calls });
	vector<Replacement> unique_replacements;
	for (const auto& replacement : replacements)
	{
		auto existing = std::find_if(
			unique_replacements.begin(),
			unique_replacements.end(),
			[&replacement](const auto& item)
			{ return item.start == replacement.start && item.end == replacement.end; });
		if (existing == unique_replacements.end())
			unique_replacements.push_back(replacement);
		else if (existing->value != replacement.value)
			return false;
	}
	std::sort(
		unique_replacements.begin(),
		unique_replacements.end(),
		[](const auto& lhs, const auto& rhs) { return lhs.start > rhs.start; });
	string updated_text = source_text_;
	for (const auto& replacement : unique_replacements)
	{
		if (replacement.start > replacement.end || replacement.end > updated_text.size())
			return false;
		updated_text.replace(replacement.start, replacement.end - replacement.start, replacement.value);
	}

	if (replacements.empty())
		return true;
	if (!entry.importMem(updated_text.data(), updated_text.size()))
		return false;
	if (entry.state() == ArchiveEntry::State::Unmodified)
		entry.setState(ArchiveEntry::State::Modified);
	game::updateCustomDefinitions();
	return true;
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