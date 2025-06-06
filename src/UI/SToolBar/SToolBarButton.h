#pragma once

namespace slade
{
class SAction;

class SToolBarButton : public wxControl
{
public:
	SToolBarButton(wxWindow* parent, const string& action, const string& icon = "", bool show_name = false);
	SToolBarButton(
		wxWindow*     parent,
		const string& action_id,
		const string& action_name,
		const string& icon,
		const string& help_text,
		bool          show_name = false,
		int           icon_size = -1);
	~SToolBarButton() = default;

	SAction*      action() const { return action_; }
	const string& actionId() const { return action_id_; }
	bool          isChecked() const;
	wxMenu*       menu() const { return menu_dropdown_; }

	void setIcon(const string& icon);
	void setChecked(bool checked);
	void setMenu(wxMenu* menu, bool delete_existing = false);

	bool updateState(int mouse_event = 0);

	static int pixelHeight();

private:
	enum class State
	{
		Normal,
		MouseOver,
		MouseDown,
	};

	SAction* action_ = nullptr;
#if wxCHECK_VERSION(3, 1, 6)
	wxBitmapBundle icon_;
#else
	wxBitmap icon_;
#endif
	State   state_         = State::Normal;
	bool    show_name_     = false;
	wxMenu* menu_dropdown_ = nullptr;

	// For non-SAction buttons
	string action_id_;
	string action_name_;
	string help_text_;
	bool   checked_ = false;

	// Layout
	int pad_outer_  = 3;
	int pad_inner_  = 1;
	int icon_size_  = 16;
	int text_width_ = 0;

	void sendClickedEvent();
	void updateSize();

	// Events
	void onPaint(wxPaintEvent& e);
	void onMouseEvent(wxMouseEvent& e);
	void onFocus(wxFocusEvent& e);
	void onEraseBackground(wxEraseEvent& e);
};
} // namespace slade

wxDECLARE_EVENT(wxEVT_STOOLBAR_BUTTON_CLICKED, wxCommandEvent);
