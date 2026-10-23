#pragma once

#include "EntryPanel.h"
#include "Game/ZScript.h"
#include "UI/Canvas/GfxCanvas.h"

namespace slade
{
class FighterEntryPanel : public EntryPanel
{
public:
	FighterEntryPanel(wxWindow* parent);
	~FighterEntryPanel() override;

protected:
	bool loadEntry(ArchiveEntry* entry) override;

private:
	struct DisplayFrame
	{
		string sprite_base;
		char   sprite_frame    = 0;
		int    duration        = 0;
		unsigned hit_line_insert_pos = 0;
		vector<zscript::State::HitLine> hit_lines;
	};

	struct DisplayState
	{
		string               name;
		vector<DisplayFrame> frames;
	};

	vector<DisplayState> states_;
	Archive*             priority_archive_ = nullptr;
	ArchiveEntry*         source_entry_     = nullptr;
	string                source_text_;
	Vec2d                actor_scale_      = { 0.2, 0.2 };
	double               actor_height_     = 53;
	GfxCanvas*           canvas_           = nullptr;
	wxChoice*            choice_state_     = nullptr;
	wxButton*            button_prev_      = nullptr;
	wxButton*            button_next_      = nullptr;
	wxButton*            button_play_      = nullptr;
	wxStaticText*        label_frame_      = nullptr;
	wxChoice*            choice_hit_line_  = nullptr;
	wxButton*            button_add_hit_line_ = nullptr;
	wxStaticText*        label_hit_line_   = nullptr;
	wxSpinCtrlDouble*    spin_hit_line_length_ = nullptr;
	wxSpinCtrlDouble*    spin_hit_line_x_      = nullptr;
	wxSpinCtrlDouble*    spin_hit_line_y_      = nullptr;
	wxFlexGridSizer*     sizer_hit_line_parameters_ = nullptr;
	vector<wxTextCtrl*>  text_hit_line_parameters_;
	size_t               current_frame_    = 1;
	size_t               current_hit_line_ = 0;
	wxTimer              playback_timer_;
	bool                 playing_ = false;
	bool                 updating_hit_line_controls_ = false;

	void updateFrame();
	void updateHitLineInfo();
	void updateHitLineOverlays();
	void setPlaying(bool playing);
	bool writeEntry(ArchiveEntry& entry) override;
	void onStateChanged(wxCommandEvent& e);
	void onHitLineChanged(wxCommandEvent& e);
	void onAddHitLine(wxCommandEvent& e);
	void onHitLineValueChanged(wxCommandEvent& e);
	void onHitLineParameterChanged(wxCommandEvent& e);
	void onPrevious(wxCommandEvent& e);
	void onNext(wxCommandEvent& e);
	void onPlay(wxCommandEvent& e);
	void onPlaybackTimer(wxTimerEvent& e);
};
} // namespace slade