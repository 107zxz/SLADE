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
		char   sprite_frame = 0;
		int    duration     = 0;
	};

	struct DisplayState
	{
		string               name;
		vector<DisplayFrame> frames;
	};

	vector<DisplayState> states_;
	Archive*             priority_archive_ = nullptr;
	GfxCanvas*           canvas_           = nullptr;
	wxChoice*            choice_state_     = nullptr;
	wxButton*            button_prev_      = nullptr;
	wxButton*            button_next_      = nullptr;
	wxButton*            button_play_      = nullptr;
	wxStaticText*        label_frame_      = nullptr;
	size_t               current_frame_    = 0;
	wxTimer              playback_timer_;
	bool                 playing_ = false;

	void updateFrame();
	void setPlaying(bool playing);
	void onStateChanged(wxCommandEvent& e);
	void onPrevious(wxCommandEvent& e);
	void onNext(wxCommandEvent& e);
	void onPlay(wxCommandEvent& e);
	void onPlaybackTimer(wxTimerEvent& e);
};
} // namespace slade