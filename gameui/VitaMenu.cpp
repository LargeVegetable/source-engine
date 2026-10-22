//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: The Vita's menu screens (see VitaMenu.h). Laid out in code, so
//          they need no resource files from the game data.
//
//=============================================================================//

#include "VitaMenu.h"
#include "BasePanel.h"
#include "BaseSaveGameDialog.h"
#include "GameUI_Interface.h"
#include "ModInfo.h"
#include "EngineInterface.h"

#include "vgui/IInput.h"
#include "vgui/ILocalize.h"
#include "vgui/ISurface.h"
#include "vgui/IVGui.h"
#include "vgui_controls/EditablePanel.h"
#include "vgui_controls/KeyRepeat.h"
#include "inputsystem/iinputsystem.h"
#include "vita_buttons.h"
#include "bitmap/tgaloader.h"
#include "filesystem.h"
#include "tier1/convar.h"
#include "tier1/strtools.h"
#include "tier1/utlstring.h"
#include "tier1/utlvector.h"
#include <time.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

using namespace vgui;

extern const char *COM_GetModDirectory( void );

//-----------------------------------------------------------------------------
// Fonts, text and button symbols
//-----------------------------------------------------------------------------
enum
{
	VITA_FONT_SMALL,	// details, footer hints, tabs
	VITA_FONT_ITEM,		// list rows, main menu items
	VITA_FONT_TITLE,	// screen titles
	VITA_FONT_COUNT
};

static HFont s_hVitaFonts[VITA_FONT_COUNT];

extern "C" const char *so_port_version( void );	// launcher: "v0.1.0-beta"
extern "C" void so_set_touch_capture( int bCapture );	// launcher: touchscreen halves act while assigning
static int s_nVitaFontsTall = -1;

// Scales a size given for 368 lines (the 640x368 render) to the screen
static int VS( int n )
{
	int wide, tall;
	surface()->GetScreenSize( wide, tall );
	return n * tall / 368;
}

static HFont VitaFont( int nFont )
{
	int wide, tall;
	surface()->GetScreenSize( wide, tall );
	if ( tall != s_nVitaFontsTall )
	{
		// DejaVu Sans on the Vita, whatever the name (vgui_surfacelib/linuxfont.cpp)
		static const int s_nTall[VITA_FONT_COUNT] = { 13, 18, 26 };
		static const int s_nWeight[VITA_FONT_COUNT] = { 500, 700, 700 };
		for ( int i = 0; i < VITA_FONT_COUNT; i++ )
		{
			if ( !s_hVitaFonts[i] )
				s_hVitaFonts[i] = surface()->CreateFont();
			surface()->SetFontGlyphSet( s_hVitaFonts[i], "Verdana", MAX( 8, s_nTall[i] * tall / 368 ), s_nWeight[i], 0, 0,
				ISurface::FONTFLAG_ANTIALIAS | ( i != VITA_FONT_SMALL ? ISurface::FONTFLAG_DROPSHADOW : 0 ) );
		}
		s_nVitaFontsTall = tall;
	}
	return s_hVitaFonts[nFont];
}

HFont VitaMenu_ItemFont()
{
	return VitaFont( VITA_FONT_ITEM );
}

// "#Token" strings are localized, others converted from UTF-8
static void VitaLocalize( const char *pszText, wchar_t *pOut, int nOutBytes )
{
	if ( pszText[0] == '#' )
	{
		const wchar_t *pLocalized = g_pVGuiLocalize->Find( pszText );
		if ( pLocalized )
		{
			V_wcsncpy( pOut, pLocalized, nOutBytes );
			return;
		}
	}
	g_pVGuiLocalize->ConvertANSIToUnicode( pszText, pOut, nOutBytes );
}

static int VitaTextWide( HFont font, const char *pszText )
{
	wchar_t wsz[256];
	VitaLocalize( pszText, wsz, sizeof( wsz ) );
	int wide, tall;
	surface()->GetTextSize( font, wsz, wide, tall );
	return wide;
}

// nAlign: -1 left, 0 centred, 1 right of x. Returns the text width
static int VitaDrawText( HFont font, int x, int y, const char *pszText, Color color, int nAlign = -1 )
{
	wchar_t wsz[256];
	VitaLocalize( pszText, wsz, sizeof( wsz ) );
	int wide, tall;
	surface()->GetTextSize( font, wsz, wide, tall );
	if ( nAlign == 0 )
		x -= wide / 2;
	else if ( nAlign > 0 )
		x -= wide;
	surface()->DrawSetTextFont( font );
	surface()->DrawSetTextColor( color );
	surface()->DrawSetTextPos( x, y );
	surface()->DrawPrintText( wsz, V_wcslen( wsz ) );
	return wide;
}

enum VitaGlyph_t
{
	VG_NONE = -1,
	VG_CROSS,
	VG_CIRCLE,
	VG_SQUARE,
	VG_TRIANGLE,
	VG_L,
	VG_R,
	VG_LR,
	VG_START,
	VG_SELECT,
	VG_DPAD,
	VG_REAR,
	VG_SCREEN,
};

static const char *VitaGlyphLabel( int nGlyph )
{
	switch ( nGlyph )
	{
	case VG_L:		return "L";
	case VG_R:		return "R";
	case VG_LR:		return "L/R";
	case VG_START:	return "START";
	case VG_SELECT:	return "SELECT";
	case VG_DPAD:	return "D-PAD";
	case VG_REAR:	return "REAR";
	case VG_SCREEN:	return "SCREEN";
	default:		return NULL;
	}
}

static int VitaGlyphWide( int nGlyph, int size )
{
	const char *pszLabel = VitaGlyphLabel( nGlyph );
	if ( !pszLabel )
		return size;
	return VitaTextWide( VitaFont( VITA_FONT_SMALL ), pszLabel ) + VS( 8 );
}

// A PlayStation button symbol in a size x size box at (x, y), or a labelled
// pill for the buttons without one. Drawn with lines: no font or texture
static void VitaDrawGlyph( int nGlyph, int x, int y, int size )
{
	const int cx = x + size / 2, cy = y + size / 2;
	const int k = size * 9 / 32;
	switch ( nGlyph )
	{
	case VG_CROSS:
	case VG_CIRCLE:
	case VG_SQUARE:
	case VG_TRIANGLE:
		surface()->DrawSetColor( 235, 235, 235, 210 );
		surface()->DrawOutlinedCircle( cx, cy, size / 2, 24 );
		break;
	default:
		break;
	}

	switch ( nGlyph )
	{
	case VG_CROSS:
		surface()->DrawSetColor( 124, 178, 232, 255 );
		for ( int t = 0; t < 2; t++ )
		{
			surface()->DrawLine( cx - k + t, cy - k, cx + k + t, cy + k );
			surface()->DrawLine( cx + k + t, cy - k, cx - k + t, cy + k );
		}
		break;
	case VG_CIRCLE:
		surface()->DrawSetColor( 255, 102, 102, 255 );
		surface()->DrawOutlinedCircle( cx, cy, k, 16 );
		surface()->DrawOutlinedCircle( cx, cy, k - 1, 16 );
		break;
	case VG_SQUARE:
		surface()->DrawSetColor( 255, 105, 248, 255 );
		surface()->DrawOutlinedRect( cx - k + 1, cy - k + 1, cx + k, cy + k );
		surface()->DrawOutlinedRect( cx - k + 2, cy - k + 2, cx + k - 1, cy + k - 1 );
		break;
	case VG_TRIANGLE:
	{
		surface()->DrawSetColor( 64, 226, 160, 255 );
		const int top = cy - k, bottom = cy + k * 3 / 4;
		for ( int t = 0; t < 2; t++ )
		{
			surface()->DrawLine( cx + t, top, cx - k + t, bottom );
			surface()->DrawLine( cx - t, top, cx + k - t, bottom );
			surface()->DrawLine( cx - k, bottom - t, cx + k, bottom - t );
		}
		break;
	}
	default:
	{
		const char *pszLabel = VitaGlyphLabel( nGlyph );
		if ( !pszLabel )
			break;
		HFont font = VitaFont( VITA_FONT_SMALL );
		const int wide = VitaGlyphWide( nGlyph, size );
		surface()->DrawSetColor( 235, 235, 235, 200 );
		surface()->DrawOutlinedRect( x, y + 1, x + wide, y + size - 1 );
		VitaDrawText( font, x + wide / 2, y + ( size - surface()->GetFontTall( font ) ) / 2, pszLabel, Color( 235, 235, 235, 255 ), 0 );
		break;
	}
	}
}

struct VitaHint_t
{
	int m_nGlyph;
	const char *m_pszText;
};

static int VitaFooterTop( int tall )
{
	return tall - VS( 32 );
}

// Button hints along the bottom right, as on the consoles
static void VitaDrawFooter( int wide, int tall, const VitaHint_t *pHints, int nHints )
{
	HFont font = VitaFont( VITA_FONT_SMALL );
	const int size = VS( 18 );
	const int y = VitaFooterTop( tall ) + VS( 6 );
	int x = wide - VS( 28 );
	for ( int i = nHints - 1; i >= 0; i-- )
	{
		x -= VitaTextWide( font, pHints[i].m_pszText );
		VitaDrawText( font, x, y + ( size - surface()->GetFontTall( font ) ) / 2, pHints[i].m_pszText, Color( 230, 230, 230, 255 ) );
		x -= VS( 5 ) + VitaGlyphWide( pHints[i].m_nGlyph, size );
		VitaDrawGlyph( pHints[i].m_nGlyph, x, y, size );
		x -= VS( 16 );
	}
}

static void VitaPlaySound( const char *pszSound )
{
	surface()->PlaySound( pszSound );
}

//-----------------------------------------------------------------------------
// The base screen: a title, a list of rows, an optional detail pane on the
// right, button hints, and a yes/no prompt
//-----------------------------------------------------------------------------
struct VitaRow_t
{
	VitaRow_t() : m_flBar( -1.0f ), m_nGlyph( VG_NONE ), m_bEnabled( true ), m_bArrows( false ) {}

	CUtlString m_Label;
	CUtlString m_Value;
	float m_flBar;		// 0..1 draws a slider bar; < 0 none
	int m_nGlyph;		// a button symbol before the value (controls)
	bool m_bEnabled;
	bool m_bArrows;		// left/right changes it
};

class CVitaScreen;
static vgui::DHANDLE< CVitaScreen > s_hVitaScreen;	// one screen at a time

class CVitaScreen : public EditablePanel
{
	DECLARE_CLASS_SIMPLE( CVitaScreen, EditablePanel );

public:
	CVitaScreen( Panel *pParent, const char *pszName, const char *pszTitle ) : BaseClass( pParent, pszName )
	{
		m_Title = pszTitle;
		m_nSel = 0;
		m_nScroll = 0;
		m_nConfirmId = 0;
		m_bConfirming = false;
		SetPaintBackgroundEnabled( true );
		SetProportional( false );
	}

	void Open()
	{
		if ( s_hVitaScreen.Get() && s_hVitaScreen.Get() != this )
			s_hVitaScreen->CloseQuietly();
		s_hVitaScreen = this;

		int wide, tall;
		surface()->GetScreenSize( wide, tall );
		SetBounds( 0, 0, wide, tall );
		MakePopup( false, false );
		SetKeyBoardInputEnabled( true );
		SetMouseInputEnabled( true );
		SetVisible( true );
		Refresh();
		m_nSel = -1;
		SetSelection( InitialRow() );
		MoveToFront();
		RequestFocus();
	}

	// Closes and hands the keys back to the main menu
	virtual void Close()
	{
		CloseQuietly();
		if ( BasePanel() )
			PostMessage( BasePanel(), new KeyValues( "Command", "command", "OpenGameMenu" ) );
	}

	void CloseQuietly()
	{
		SetVisible( false );
		MarkForDeletion();
		if ( s_hVitaScreen.Get() == this )
			s_hVitaScreen = NULL;
	}

protected:
	// what screens fill in
	virtual void Refresh() = 0;		// rebuilds m_Rows (and m_Hints)
	virtual void OnActivateRow( int nRow ) {}
	virtual void OnAdjustRow( int nRow, int nDir ) {}
	virtual void OnSquare( int nRow ) {}
	virtual void OnTriangle( int nRow ) {}
	virtual void OnShoulder( int nDir ) {}
	virtual void OnBack() { VitaPlaySound( "UI/buttonclickrelease.wav" ); Close(); }
	virtual void OnSelectionChanged() {}
	virtual void OnConfirmed( int nId ) {}
	virtual bool OnCaptureKey( KeyCode code ) { return false; }	// true: taken (button capture)
	virtual bool HasDetail() const { return false; }
	virtual void PaintDetail( int x, int y, int wide, int tall ) {}
	virtual int HeaderTall() const { return 0; }
	virtual void PaintHeader( int x, int y, int wide ) {}
	virtual void OnTapHeader( int x ) {}

	void Confirm( const char *pszQuestion, int nId )
	{
		m_Question = pszQuestion;
		m_nConfirmId = nId;
		m_bConfirming = true;
	}

	virtual int InitialRow() const { return FirstEnabledRow(); }

	int FirstEnabledRow() const
	{
		for ( int i = 0; i < m_Rows.Count(); i++ )
		{
			if ( m_Rows[i].m_bEnabled )
				return i;
		}
		return 0;
	}

	void SetSelection( int nRow )
	{
		if ( !m_Rows.Count() )
		{
			m_nSel = 0;
			m_nScroll = 0;
			return;
		}
		nRow = clamp( nRow, 0, m_Rows.Count() - 1 );
		const bool bChanged = ( nRow != m_nSel );
		m_nSel = nRow;
		const int nVisible = VisibleRows();
		if ( m_nSel < m_nScroll )
			m_nScroll = m_nSel;
		else if ( m_nSel >= m_nScroll + nVisible )
			m_nScroll = m_nSel - nVisible + 1;
		m_nScroll = clamp( m_nScroll, 0, MAX( 0, m_Rows.Count() - nVisible ) );
		if ( bChanged )
			OnSelectionChanged();
	}

	void GetListRect( int &x, int &y, int &wide, int &tall ) const
	{
		int sw, st;
		GetSize( sw, st );
		const int margin = VS( 28 );
		x = margin + VS( 8 );
		y = VS( 16 ) + surface()->GetFontTall( VitaFont( VITA_FONT_TITLE ) ) + VS( 14 ) + HeaderTall();
		wide = ( HasDetail() ? sw * 56 / 100 : sw - margin ) - x;
		tall = VitaFooterTop( st ) - VS( 6 ) - y;
	}

	void GetDetailRect( int &x, int &y, int &wide, int &tall ) const
	{
		int lx, ly, lw, lh, sw, st;
		GetListRect( lx, ly, lw, lh );
		GetSize( sw, st );
		x = sw * 60 / 100;
		y = ly;
		wide = sw - VS( 28 ) - x;
		tall = lh;
	}

	int RowTall() const { return VS( 24 ); }
	int VisibleRows() const
	{
		int x, y, wide, tall;
		GetListRect( x, y, wide, tall );
		return MAX( 1, tall / RowTall() );
	}

	CUtlString m_Title;
	CUtlString m_EmptyText;		// shown when there are no rows
	CUtlVector< VitaRow_t > m_Rows;
	CUtlVector< VitaHint_t > m_Hints;
	int m_nSel;
	int m_nScroll;

	virtual void PaintBackground()
	{
		int wide, tall;
		GetSize( wide, tall );
		surface()->DrawSetColor( 0, 0, 0, 218 );
		surface()->DrawFilledRect( 0, 0, wide, tall );
		// a faint band behind the title, as on the consoles
		surface()->DrawSetColor( 255, 255, 255, 255 );
		surface()->DrawFilledRectFade( 0, 0, wide, VS( 56 ), 18, 0, false );
	}

	virtual void Paint()
	{
		int wide, tall;
		GetSize( wide, tall );
		const int margin = VS( 28 );

		HFont titleFont = VitaFont( VITA_FONT_TITLE );
		VitaDrawText( titleFont, margin, VS( 16 ), m_Title.Get(), Color( 255, 255, 255, 255 ) );
		{
			// the port's version, for bug reports (launcher_vita/version.h)
			HFont smallFont = VitaFont( VITA_FONT_SMALL );
			const char *pszVersion = so_port_version();
			VitaDrawText( smallFont, wide - margin - VitaTextWide( smallFont, pszVersion ), VS( 22 ), pszVersion, Color( 255, 255, 255, 90 ) );
		}
		const int lineY = VS( 16 ) + surface()->GetFontTall( titleFont ) + VS( 4 );
		surface()->DrawSetColor( 255, 255, 255, 70 );
		surface()->DrawFilledRect( margin, lineY, wide - margin, lineY + 1 );
		if ( HeaderTall() )
			PaintHeader( margin, lineY + VS( 6 ), wide - 2 * margin );

		int lx, ly, lw, lh;
		GetListRect( lx, ly, lw, lh );
		HFont itemFont = VitaFont( VITA_FONT_ITEM );
		const int rowTall = RowTall();
		const int fontTall = surface()->GetFontTall( itemFont );
		const int nVisible = VisibleRows();

		if ( !m_Rows.Count() && !m_EmptyText.IsEmpty() )
			VitaDrawText( itemFont, lx, ly + ( rowTall - fontTall ) / 2, m_EmptyText.Get(), Color( 150, 150, 150, 255 ) );

		for ( int i = m_nScroll; i < m_Rows.Count() && i < m_nScroll + nVisible; i++ )
		{
			const VitaRow_t &row = m_Rows[i];
			const int ry = ly + ( i - m_nScroll ) * rowTall;
			const bool bSel = ( i == m_nSel );
			if ( bSel )
			{
				surface()->DrawSetColor( 255, 255, 255, 34 );
				surface()->DrawFilledRect( lx - VS( 8 ), ry, lx + lw, ry + rowTall );
				surface()->DrawSetColor( 255, 176, 0, 255 );
				surface()->DrawFilledRect( lx - VS( 8 ), ry, lx - VS( 5 ), ry + rowTall );
			}

			Color textColor = !row.m_bEnabled ? Color( 110, 110, 110, 255 ) : ( bSel ? Color( 255, 255, 255, 255 ) : Color( 200, 200, 200, 255 ) );
			const int ty = ry + ( rowTall - fontTall ) / 2;
			VitaDrawText( itemFont, lx, ty, row.m_Label.Get(), textColor );

			int vx = lx + lw - VS( 8 );
			if ( row.m_bArrows && bSel )
				vx -= VitaDrawText( itemFont, vx, ty, ">", Color( 255, 176, 0, 255 ), 1 ) + VS( 6 );
			if ( row.m_flBar >= 0.0f )
			{
				const int bw = lw * 28 / 100, bh = VS( 8 );
				const int bx = vx - bw, by = ry + ( rowTall - bh ) / 2;
				surface()->DrawSetColor( 255, 255, 255, 48 );
				surface()->DrawFilledRect( bx, by, bx + bw, by + bh );
				surface()->DrawSetColor( 255, 176, 0, 230 );
				surface()->DrawFilledRect( bx, by, bx + (int)( bw * clamp( row.m_flBar, 0.0f, 1.0f ) ), by + bh );
				vx = bx - VS( 8 );
			}
			if ( !row.m_Value.IsEmpty() )
				vx -= VitaDrawText( itemFont, vx, ty, row.m_Value.Get(), textColor, 1 ) + VS( 6 );
			if ( row.m_nGlyph != VG_NONE )
			{
				const int gs = VS( 18 );
				const int gw = VitaGlyphWide( row.m_nGlyph, gs );
				VitaDrawGlyph( row.m_nGlyph, vx - gw, ry + ( rowTall - gs ) / 2, gs );
				vx -= gw + VS( 6 );
			}
			if ( row.m_bArrows && bSel )
				VitaDrawText( itemFont, vx, ty, "<", Color( 255, 176, 0, 255 ), 1 );
		}

		// more rows above / below
		surface()->DrawSetColor( 255, 176, 0, 200 );
		const int ax = lx + lw - VS( 4 );
		if ( m_nScroll > 0 )
		{
			surface()->DrawLine( ax - VS( 5 ), ly - VS( 3 ), ax, ly - VS( 8 ) );
			surface()->DrawLine( ax, ly - VS( 8 ), ax + VS( 5 ), ly - VS( 3 ) );
		}
		if ( m_nScroll + nVisible < m_Rows.Count() )
		{
			const int by = ly + nVisible * rowTall + VS( 3 );
			surface()->DrawLine( ax - VS( 5 ), by, ax, by + VS( 5 ) );
			surface()->DrawLine( ax, by + VS( 5 ), ax + VS( 5 ), by );
		}

		if ( HasDetail() )
		{
			int dx, dy, dw, dh;
			GetDetailRect( dx, dy, dw, dh );
			PaintDetail( dx, dy, dw, dh );
		}

		if ( m_bConfirming )
		{
			PaintConfirm( wide, tall );
		}
		else
		{
			VitaDrawFooter( wide, tall, m_Hints.Base(), m_Hints.Count() );
		}
	}

	virtual void OnKeyCodePressed( KeyCode code )
	{
		m_KeyRepeat.KeyDown( code );
		HandleKey( code );
	}

	virtual void OnKeyCodeReleased( KeyCode code )
	{
		m_KeyRepeat.KeyUp( code );
		BaseClass::OnKeyCodeReleased( code );
	}

	virtual void OnThink()
	{
		KeyCode code = m_KeyRepeat.KeyRepeated();
		if ( code )
			HandleKey( code );
		BaseClass::OnThink();
	}

	virtual void OnKillFocus()
	{
		m_KeyRepeat.Reset();
		BaseClass::OnKillFocus();
	}

	// touch (the front panel acts as the mouse): tap a row to select it, tap
	// it again to use it; tap the hints to go back
	virtual void OnMousePressed( MouseCode code )
	{
		if ( code != MOUSE_LEFT )
			return;
		int mx, my;
		input()->GetCursorPos( mx, my );
		ScreenToLocal( mx, my );
		int wide, tall;
		GetSize( wide, tall );

		if ( m_bConfirming )
		{
			// left half yes, right half no
			m_bConfirming = false;
			if ( mx < wide / 2 )
				OnConfirmed( m_nConfirmId );
			return;
		}
		if ( my >= VitaFooterTop( tall ) )
		{
			OnBack();
			return;
		}

		int lx, ly, lw, lh;
		GetListRect( lx, ly, lw, lh );
		if ( HeaderTall() && my < ly && my >= ly - HeaderTall() - VS( 8 ) )
		{
			OnTapHeader( mx );
			return;
		}
		if ( mx >= lx - VS( 8 ) && mx < lx + lw && my >= ly && my < ly + lh )
		{
			const int nRow = m_nScroll + ( my - ly ) / RowTall();
			if ( nRow < m_Rows.Count() && m_Rows[nRow].m_bEnabled )
			{
				if ( nRow == m_nSel )
				{
					VitaPlaySound( "UI/buttonclick.wav" );
					if ( m_Rows[nRow].m_bArrows )
						OnAdjustRow( nRow, mx >= lx + lw / 2 ? 1 : -1 );
					else
						OnActivateRow( nRow );
				}
				else
				{
					VitaPlaySound( "UI/buttonrollover.wav" );
					SetSelection( nRow );
				}
			}
		}
	}

private:
	void HandleKey( KeyCode code )
	{
		if ( OnCaptureKey( code ) )
			return;

		if ( m_bConfirming )
		{
			if ( code == KEY_XBUTTON_A || code == KEY_ENTER )
			{
				m_bConfirming = false;
				VitaPlaySound( "UI/buttonclick.wav" );
				OnConfirmed( m_nConfirmId );
			}
			else if ( code == KEY_XBUTTON_B || code == KEY_ESCAPE )
			{
				m_bConfirming = false;
				VitaPlaySound( "UI/buttonclickrelease.wav" );
			}
			return;
		}

		const bool bValid = m_Rows.IsValidIndex( m_nSel ) && m_Rows[m_nSel].m_bEnabled;
		switch ( code )
		{
		case KEY_XBUTTON_UP:
		case KEY_XSTICK1_UP:
		case KEY_UP:
			Move( -1 );
			break;
		case KEY_XBUTTON_DOWN:
		case KEY_XSTICK1_DOWN:
		case KEY_DOWN:
			Move( 1 );
			break;
		case KEY_XBUTTON_LEFT:
		case KEY_XSTICK1_LEFT:
		case KEY_LEFT:
			if ( bValid && m_Rows[m_nSel].m_bArrows )
			{
				VitaPlaySound( "UI/buttonrollover.wav" );
				OnAdjustRow( m_nSel, -1 );
			}
			break;
		case KEY_XBUTTON_RIGHT:
		case KEY_XSTICK1_RIGHT:
		case KEY_RIGHT:
			if ( bValid && m_Rows[m_nSel].m_bArrows )
			{
				VitaPlaySound( "UI/buttonrollover.wav" );
				OnAdjustRow( m_nSel, 1 );
			}
			break;
		case KEY_XBUTTON_A:
		case KEY_ENTER:
			if ( bValid )
			{
				VitaPlaySound( "UI/buttonclick.wav" );
				OnActivateRow( m_nSel );
			}
			break;
		case KEY_XBUTTON_B:
		case KEY_ESCAPE:
			OnBack();
			break;
		case KEY_XBUTTON_X:
			if ( bValid )
				OnSquare( m_nSel );
			break;
		case KEY_XBUTTON_Y:
			if ( bValid )
				OnTriangle( m_nSel );
			break;
		case KEY_XBUTTON_LEFT_SHOULDER:
			OnShoulder( -1 );
			break;
		case KEY_XBUTTON_RIGHT_SHOULDER:
			OnShoulder( 1 );
			break;
		default:
			BaseClass::OnKeyCodePressed( code );
			break;
		}
	}

	void Move( int nDir )
	{
		int i = m_nSel + nDir;
		while ( i >= 0 && i < m_Rows.Count() && !m_Rows[i].m_bEnabled )
			i += nDir;
		if ( i >= 0 && i < m_Rows.Count() && i != m_nSel )
		{
			VitaPlaySound( "UI/buttonrollover.wav" );
			SetSelection( i );
		}
	}

	void PaintConfirm( int wide, int tall )
	{
		HFont itemFont = VitaFont( VITA_FONT_ITEM );
		const int bw = wide * 70 / 100, bh = VS( 110 );
		const int bx = ( wide - bw ) / 2, by = ( tall - bh ) / 2;
		surface()->DrawSetColor( 18, 18, 18, 248 );
		surface()->DrawFilledRect( bx, by, bx + bw, by + bh );
		surface()->DrawSetColor( 255, 176, 0, 220 );
		surface()->DrawOutlinedRect( bx, by, bx + bw, by + bh );

		// the question, one line per '\n'
		char szLines[256];
		V_strncpy( szLines, m_Question.Get(), sizeof( szLines ) );
		int y = by + VS( 14 );
		for ( char *pLine = szLines; pLine; )
		{
			char *pNext = strchr( pLine, '\n' );
			if ( pNext )
				*pNext++ = 0;
			VitaDrawText( itemFont, wide / 2, y, pLine, Color( 255, 255, 255, 255 ), 0 );
			y += surface()->GetFontTall( itemFont ) + VS( 4 );
			pLine = pNext;
		}

		static const VitaHint_t s_YesNo[] = { { VG_CROSS, "Yes" }, { VG_CIRCLE, "No" } };
		// along the box's bottom edge (the footer sits VS( 26 ) above "tall")
		VitaDrawFooter( bx + bw / 2 + VS( 80 ), by + bh - VS( 4 ), s_YesNo, 2 );
	}

	CKeyRepeatHandler m_KeyRepeat;
	CUtlString m_Question;
	int m_nConfirmId;
	bool m_bConfirming;
};

//-----------------------------------------------------------------------------
// Save games
//-----------------------------------------------------------------------------

// CBaseSaveGameDialog::ParseSaveData, with a short date
static bool VitaParseSave( const char *pszFileName, const char *pszShortName, SaveGameDescription_t &save )
{
	memset( &save, 0, sizeof( save ) );
	V_strncpy( save.szShortName, pszShortName, sizeof( save.szShortName ) );
	V_strncpy( save.szFileName, pszFileName, sizeof( save.szFileName ) );

	FileHandle_t fh = g_pFullFileSystem->Open( pszFileName, "rb", "MOD" );
	if ( fh == FILESYSTEM_INVALID_HANDLE )
		return false;
	char szMapName[SAVEGAME_MAPNAME_LEN];
	char szComment[SAVEGAME_COMMENT_LEN];
	const int bRead = SaveReadNameAndComment( fh, szMapName, sizeof( szMapName ), szComment, sizeof( szComment ) );
	g_pFullFileSystem->Close( fh );
	if ( !bRead )
		return false;
	V_strncpy( save.szMapName, szMapName, sizeof( save.szMapName ) );

	// the comment is "<chapter title> mmm:ss" (play time)
	int nLen = V_strlen( szComment );
	V_strncpy( save.szElapsedTime, "", sizeof( save.szElapsedTime ) );
	if ( nLen >= 6 )
	{
		const int nMinutes = atoi( &szComment[nLen - 6] );
		const int nSeconds = atoi( &szComment[nLen - 2] );
		if ( nMinutes >= 60 )
			V_snprintf( save.szElapsedTime, sizeof( save.szElapsedTime ), "%d h %02d min", nMinutes / 60, nMinutes % 60 );
		else
			V_snprintf( save.szElapsedTime, sizeof( save.szElapsedTime ), "%d min %02d s", nMinutes, nSeconds );
		int n = nLen - 6;
		szComment[n] = 0;
		while ( n >= 1 && szComment[n - 1] == ' ' )
			szComment[--n] = 0;
	}
	V_strncpy( save.szComment, szComment, sizeof( save.szComment ) );

	const char *pszType = "";
	if ( V_stristr( pszFileName, "quick" ) )
		pszType = "Quicksave";
	else if ( V_stristr( pszFileName, "autosave" ) )
		pszType = "Autosave";
	V_strncpy( save.szType, pszType, sizeof( save.szType ) );

	const time_t fileTime = g_pFullFileSystem->GetFileTime( pszFileName, "MOD" );
	save.iTimestamp = (unsigned int)fileTime;
	struct tm *pTime = localtime( &fileTime );
	if ( pTime )
		strftime( save.szFileTime, sizeof( save.szFileTime ), "%b %d  %H:%M", pTime );
	return true;
}

static void VitaDeleteSave( const char *pszFileName )
{
	if ( !pszFileName || !pszFileName[0] )
		return;
	g_pFullFileSystem->RemoveFile( pszFileName, "MOD" );
	char szTGA[MAX_PATH];
	V_strncpy( szTGA, pszFileName, sizeof( szTGA ) );
	char *pExt = V_stristr( szTGA, ".sav" );
	if ( pExt )
	{
		V_strcpy( pExt, ".tga" );
		g_pFullFileSystem->RemoveFile( szTGA, "MOD" );
	}
}

// CSaveGameDialog::FindSaveSlot: the first free half-life-NNN
static void VitaFindSaveSlot( char *pszOut, int nOutSize )
{
	pszOut[0] = 0;
	for ( int i = 0; i < 1000; i++ )
	{
		char szFileName[MAX_PATH];
		V_snprintf( szFileName, sizeof( szFileName ), "save/half-life-%03i.sav", i );
		if ( !g_pFullFileSystem->FileExists( szFileName, "MOD" ) )
		{
			V_snprintf( pszOut, nOutSize, "half-life-%03i", i );
			return;
		}
	}
}

class CVitaSaveLoadScreen : public CVitaScreen
{
	DECLARE_CLASS_SIMPLE( CVitaSaveLoadScreen, CVitaScreen );

public:
	CVitaSaveLoadScreen( Panel *pParent, bool bSave ) : BaseClass( pParent, "VitaSaveLoad", bSave ? "SAVE GAME" : "LOAD GAME" )
	{
		m_bSave = bSave;
		m_nThumbTexture = -1;
		m_nThumbWide = m_nThumbTall = 0;
		m_nThumbFor = -2;
		m_bThumbValid = false;
		m_EmptyText = "No saved games";
	}

	~CVitaSaveLoadScreen()
	{
		if ( surface() && m_nThumbTexture != -1 )
			surface()->DestroyTextureID( m_nThumbTexture );
	}

protected:
	enum { CONFIRM_OVERWRITE = 1, CONFIRM_DELETE };

	virtual void Refresh()
	{
		ScanSaves();
		m_Rows.RemoveAll();
		if ( m_bSave )
		{
			VitaRow_t &row = m_Rows[m_Rows.AddToTail()];
			row.m_Label = "New save";
		}
		for ( int i = 0; i < m_Saves.Count(); i++ )
		{
			const SaveGameDescription_t &save = m_Saves[i];
			VitaRow_t &row = m_Rows[m_Rows.AddToTail()];
			row.m_Label = save.szComment[0] ? save.szComment : save.szMapName;
			row.m_Value = save.szType[0] ? save.szType : save.szFileTime;
		}
		m_Hints.RemoveAll();
		const VitaHint_t hintDelete = { VG_TRIANGLE, "Delete" };
		const VitaHint_t hintUse = { VG_CROSS, m_bSave ? "Save" : "Load" };
		const VitaHint_t hintBack = { VG_CIRCLE, "Back" };
		if ( m_Saves.Count() )
			m_Hints.AddToTail( hintDelete );
		if ( m_Rows.Count() )
			m_Hints.AddToTail( hintUse );
		m_Hints.AddToTail( hintBack );
		m_nThumbFor = -2;
	}

	const SaveGameDescription_t *SaveForRow( int nRow ) const
	{
		const int i = m_bSave ? nRow - 1 : nRow;
		return m_Saves.IsValidIndex( i ) ? &m_Saves[i] : NULL;
	}

	virtual void OnActivateRow( int nRow )
	{
		const SaveGameDescription_t *pSave = SaveForRow( nRow );
		if ( m_bSave )
		{
			if ( pSave )
				Confirm( "Overwrite this save?", CONFIRM_OVERWRITE );
			else
				SaveNew( NULL );
		}
		else if ( pSave )
		{
			char szCmd[256];
			V_snprintf( szCmd, sizeof( szCmd ), "progress_enable\nload %s\n", pSave->szShortName );
			engine->ClientCmd_Unrestricted( szCmd );
			CloseQuietly();
		}
	}

	virtual void OnTriangle( int nRow )
	{
		if ( SaveForRow( nRow ) )
			Confirm( "Delete this save?", CONFIRM_DELETE );
	}

	virtual void OnConfirmed( int nId )
	{
		const SaveGameDescription_t *pSave = SaveForRow( m_nSel );
		if ( !pSave )
			return;
		if ( nId == CONFIRM_OVERWRITE )
		{
			SaveNew( pSave->szFileName );
		}
		else if ( nId == CONFIRM_DELETE )
		{
			VitaDeleteSave( pSave->szFileName );
			const int nSel = m_nSel;
			Refresh();
			m_nSel = -1;
			SetSelection( MIN( nSel, m_Rows.Count() - 1 ) );
		}
	}

	void SaveNew( const char *pszReplace )
	{
		if ( pszReplace )
			VitaDeleteSave( pszReplace );
		char szName[64];
		VitaFindSaveSlot( szName, sizeof( szName ) );
		if ( !szName[0] )
			return;
		char szCmd[256];
		V_snprintf( szCmd, sizeof( szCmd ), "save %s\n", szName );
		engine->ClientCmd_Unrestricted( szCmd );
		CloseQuietly();
		GameUI().HideGameUI();
	}

	virtual bool HasDetail() const { return true; }

	virtual void OnSelectionChanged()
	{
		m_nThumbFor = -2;	// reload on the next paint
	}

	void LoadThumb()
	{
		m_nThumbFor = m_nSel;
		m_bThumbValid = false;
		const SaveGameDescription_t *pSave = SaveForRow( m_nSel );
		if ( !pSave )
			return;
		char szTGA[MAX_PATH];
		V_snprintf( szTGA, sizeof( szTGA ), "//MOD/%s", pSave->szFileName );
		char *pExt = V_stristr( szTGA, ".sav" );
		if ( !pExt )
			return;
		V_strcpy( pExt, ".tga" );
		CUtlMemory< unsigned char > tga;
		if ( !TGALoader::LoadRGBA8888( szTGA, tga, m_nThumbWide, m_nThumbTall ) )
			return;
		if ( m_nThumbTexture == -1 )
			m_nThumbTexture = surface()->CreateNewTextureID( true );
		surface()->DrawSetTextureRGBA( m_nThumbTexture, tga.Base(), m_nThumbWide, m_nThumbTall, true, true );
		m_bThumbValid = true;
	}

	virtual void PaintDetail( int x, int y, int wide, int tall )
	{
		if ( m_nThumbFor != m_nSel )
			LoadThumb();

		// a 16:9 frame for the screenshot
		const int frameTall = wide * 9 / 16;
		surface()->DrawSetColor( 255, 255, 255, 40 );
		surface()->DrawOutlinedRect( x - 1, y - 1, x + wide + 1, y + frameTall + 1 );
		HFont small = VitaFont( VITA_FONT_SMALL );
		HFont item = VitaFont( VITA_FONT_ITEM );
		const SaveGameDescription_t *pSave = SaveForRow( m_nSel );
		if ( m_bThumbValid && m_nThumbWide > 0 && m_nThumbTall > 0 )
		{
			int dw = wide, dt = wide * m_nThumbTall / m_nThumbWide;
			if ( dt > frameTall )
			{
				dt = frameTall;
				dw = frameTall * m_nThumbWide / m_nThumbTall;
			}
			const int dx = x + ( wide - dw ) / 2, dy = y + ( frameTall - dt ) / 2;
			surface()->DrawSetTexture( m_nThumbTexture );
			surface()->DrawSetColor( 255, 255, 255, 255 );
			surface()->DrawTexturedRect( dx, dy, dx + dw, dy + dt );
		}
		else
		{
			VitaDrawText( small, x + wide / 2, y + frameTall / 2 - VS( 6 ),
				pSave ? "No picture" : ( m_bSave && m_nSel == 0 ? "A new save of your current game" : "" ), Color( 140, 140, 140, 255 ), 0 );
		}
		if ( !pSave )
			return;

		int ty = y + frameTall + VS( 10 );
		VitaDrawText( item, x, ty, pSave->szComment[0] ? pSave->szComment : pSave->szMapName, Color( 255, 255, 255, 255 ) );
		ty += surface()->GetFontTall( item ) + VS( 4 );
		if ( pSave->szType[0] )
		{
			VitaDrawText( small, x, ty, pSave->szType, Color( 255, 176, 0, 255 ) );
			ty += surface()->GetFontTall( small ) + VS( 2 );
		}
		VitaDrawText( small, x, ty, pSave->szFileTime, Color( 200, 200, 200, 255 ) );
		ty += surface()->GetFontTall( small ) + VS( 2 );
		if ( pSave->szElapsedTime[0] )
		{
			char szPlayed[64];
			V_snprintf( szPlayed, sizeof( szPlayed ), "Played %s", pSave->szElapsedTime );
			VitaDrawText( small, x, ty, szPlayed, Color( 200, 200, 200, 255 ) );
		}
	}

private:
	void ScanSaves()
	{
		m_Saves.RemoveAll();
		FileFindHandle_t handle;
		const char *pFileName = g_pFullFileSystem->FindFirst( "save/*.sav", &handle );
		while ( pFileName )
		{
			if ( V_strnicmp( pFileName, "HLSave", 6 ) )
			{
				char szFileName[MAX_PATH];
				V_snprintf( szFileName, sizeof( szFileName ), "save/%s", pFileName );
				SaveGameDescription_t save;
				if ( g_pFullFileSystem->FileExists( szFileName, "MOD" ) && VitaParseSave( szFileName, pFileName, save ) )
					m_Saves.AddToTail( save );
			}
			pFileName = g_pFullFileSystem->FindNext( handle );
		}
		g_pFullFileSystem->FindClose( handle );
		qsort( m_Saves.Base(), m_Saves.Count(), sizeof( SaveGameDescription_t ), &CBaseSaveGameDialog::SaveGameSortFunc );
	}

	CUtlVector< SaveGameDescription_t > m_Saves;
	bool m_bSave;
	int m_nThumbTexture;
	int m_nThumbWide, m_nThumbTall;
	int m_nThumbFor;
	bool m_bThumbValid;
};

//-----------------------------------------------------------------------------
// New game: the unlocked chapters, with the difficulty on the first row
// (none in games without one, nodifficulty in gameinfo.txt: Portal)
//-----------------------------------------------------------------------------
struct VitaChapter_t
{
	char m_szFile[64];		// chapter1.cfg
	char m_szId[16];		// 1, 9a
};

static int VitaChapterSort( const void *a, const void *b )
{
	const VitaChapter_t *pA = (const VitaChapter_t *)a, *pB = (const VitaChapter_t *)b;
	const int nA = atoi( pA->m_szId ), nB = atoi( pB->m_szId );
	if ( nA != nB )
		return nA - nB;
	return V_stricmp( pA->m_szId, pB->m_szId );
}

static const char *s_pszVitaSkills[] = { "Easy", "Normal", "Hard" };

class CVitaNewGameScreen : public CVitaScreen
{
	DECLARE_CLASS_SIMPLE( CVitaNewGameScreen, CVitaScreen );

public:
	CVitaNewGameScreen( Panel *pParent ) : BaseClass( pParent, "VitaNewGame", "NEW GAME" )
	{
		// without a difficulty setting the game runs at skill 1, as on PC
		// (where nothing changes it) and the Xbox 360
		m_bDifficulty = !ModInfo().NoDifficulty();
		ConVarRef skill( "skill" );
		m_nSkill = m_bDifficulty ? clamp( skill.IsValid() ? skill.GetInt() : 2, 1, 3 ) : 1;
		m_nImageTexture = -1;
		m_nImageFor = -2;
		ScanChapters();
	}

	~CVitaNewGameScreen()
	{
		if ( surface() && m_nImageTexture != -1 )
			surface()->DestroyTextureID( m_nImageTexture );
	}

protected:
	enum { CONFIRM_START = 1 };

	virtual void Refresh()
	{
		m_Rows.RemoveAll();
		if ( m_bDifficulty )
		{
			VitaRow_t &row = m_Rows[m_Rows.AddToTail()];
			row.m_Label = "Difficulty";
			row.m_Value = s_pszVitaSkills[m_nSkill - 1];
			row.m_bArrows = true;
		}
		ConVarRef unlocked( "sv_unlockedchapters" );
		const int nUnlocked = MAX( 1, unlocked.IsValid() ? unlocked.GetInt() : 1 );
		for ( int i = 0; i < m_Chapters.Count(); i++ )
		{
			VitaRow_t &row = m_Rows[m_Rows.AddToTail()];
			char szToken[64];
			V_snprintf( szToken, sizeof( szToken ), "#%s_Chapter%s_Title", COM_GetModDirectory(), m_Chapters[i].m_szId );
			row.m_Label = szToken;
			char szValue[32];
			V_snprintf( szValue, sizeof( szValue ), "Chapter %s", m_Chapters[i].m_szId );
			row.m_Value = szValue;
			row.m_bEnabled = ( i + 1 ) <= nUnlocked;
			if ( !row.m_bEnabled )
				row.m_Label = "Locked";
		}
		m_Hints.RemoveAll();
		const VitaHint_t hints[] = { { VG_DPAD, "Difficulty" }, { VG_CROSS, "Start" }, { VG_CIRCLE, "Back" } };
		if ( m_bDifficulty )
			m_Hints.AddMultipleToTail( 3, hints );
		else
			m_Hints.AddMultipleToTail( 2, hints + 1 );
	}

	// the first chapter, as on the consoles (the difficulty is row 0)
	virtual int InitialRow() const { return m_Rows.Count() > FirstChapterRow() ? FirstChapterRow() : 0; }

	virtual void OnAdjustRow( int nRow, int nDir )
	{
		if ( m_bDifficulty && nRow == 0 )
			ChangeSkill( nDir );
	}

	virtual void OnActivateRow( int nRow )
	{
		if ( m_bDifficulty && nRow == 0 )
		{
			ChangeSkill( 1 );
			return;
		}
		if ( GameUI().IsInLevel() )
			Confirm( "Start a new game?\nProgress since your last save will be lost.", CONFIRM_START );
		else
			Start();
	}

	virtual void OnConfirmed( int nId )
	{
		if ( nId == CONFIRM_START )
			Start();
	}

	virtual bool HasDetail() const { return true; }

	virtual void PaintDetail( int x, int y, int wide, int tall )
	{
		const int nChapter = MAX( m_nSel - FirstChapterRow(), 0 );
		if ( !m_Chapters.IsValidIndex( nChapter ) || !m_Rows.IsValidIndex( nChapter + FirstChapterRow() ) || !m_Rows[nChapter + FirstChapterRow()].m_bEnabled )
			return;
		if ( m_nImageFor != nChapter )
		{
			m_nImageFor = nChapter;
			if ( m_nImageTexture == -1 )
				m_nImageTexture = surface()->CreateNewTextureID();
			char szImage[MAX_PATH];
			V_snprintf( szImage, sizeof( szImage ), "vgui/chapters/chapter%s", m_Chapters[nChapter].m_szId );
			surface()->DrawSetTextureFile( m_nImageTexture, szImage, true, false );
		}
		int iw = 0, it = 0;
		surface()->DrawGetTextureSize( m_nImageTexture, iw, it );
		if ( iw <= 0 || it <= 0 )
			return;
		const int dt = MIN( wide * it / iw, tall * 2 / 3 );
		const int dw = dt * iw / it;
		surface()->DrawSetTexture( m_nImageTexture );
		surface()->DrawSetColor( 255, 255, 255, 255 );
		surface()->DrawTexturedRect( x, y, x + dw, y + dt );
		surface()->DrawSetColor( 255, 255, 255, 40 );
		surface()->DrawOutlinedRect( x - 1, y - 1, x + dw + 1, y + dt + 1 );

		HFont item = VitaFont( VITA_FONT_ITEM );
		VitaDrawText( item, x, y + dt + VS( 10 ), m_Rows[nChapter + FirstChapterRow()].m_Label.Get(), Color( 255, 255, 255, 255 ) );
		if ( !m_bDifficulty )
			return;
		char szSkill[64];
		V_snprintf( szSkill, sizeof( szSkill ), "Difficulty: %s", s_pszVitaSkills[m_nSkill - 1] );
		VitaDrawText( VitaFont( VITA_FONT_SMALL ), x, y + dt + VS( 14 ) + surface()->GetFontTall( item ), szSkill, Color( 200, 200, 200, 255 ) );
	}

private:
	int FirstChapterRow() const { return m_bDifficulty ? 1 : 0; }

	void ChangeSkill( int nDir )
	{
		m_nSkill = clamp( m_nSkill + nDir, 1, 3 );
		m_Rows[0].m_Value = s_pszVitaSkills[m_nSkill - 1];
	}

	void Start()
	{
		const int nChapter = m_nSel - FirstChapterRow();
		if ( !m_Chapters.IsValidIndex( nChapter ) )
			return;
		ConVarRef commentary( "commentary" );
		if ( commentary.IsValid() )
			commentary.SetValue( 0 );
		ConVarRef sv_cheats( "sv_cheats" );
		if ( sv_cheats.IsValid() )
			sv_cheats.SetValue( 0 );
		char szCmd[256];
		V_snprintf( szCmd, sizeof( szCmd ), "disconnect\ndeathmatch 0\nprogress_enable\nskill %d\nexec %s\n", m_nSkill, m_Chapters[nChapter].m_szFile );
		CloseQuietly();
		BasePanel()->FadeToBlackAndRunEngineCommand( szCmd );
	}

	// CNewGameDialog's scan: the non-empty cfg/chapter*.cfg of this game
	void ScanChapters()
	{
		FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
		const char *pFileName = g_pFullFileSystem->FindFirst( "cfg/chapter*.cfg", &handle );
		while ( pFileName && m_Chapters.Count() < 32 )
		{
			char szFullName[MAX_PATH];
			V_snprintf( szFullName, sizeof( szFullName ), "cfg/%s", pFileName );
			FileHandle_t f = g_pFullFileSystem->Open( szFullName, "rb", "MOD" );
			if ( f )
			{
				if ( g_pFullFileSystem->Size( f ) > 0 )
				{
					VitaChapter_t &chapter = m_Chapters[m_Chapters.AddToTail()];
					V_strncpy( chapter.m_szFile, pFileName, sizeof( chapter.m_szFile ) );
					V_strncpy( chapter.m_szId, pFileName + 7, sizeof( chapter.m_szId ) );	// after "chapter"
					char *pExt = V_stristr( chapter.m_szId, ".cfg" );
					if ( pExt )
						*pExt = 0;
				}
				g_pFullFileSystem->Close( f );
			}
			pFileName = g_pFullFileSystem->FindNext( handle );
		}
		g_pFullFileSystem->FindClose( handle );
		qsort( m_Chapters.Base(), m_Chapters.Count(), sizeof( VitaChapter_t ), &VitaChapterSort );
	}

	CUtlVector< VitaChapter_t > m_Chapters;
	bool m_bDifficulty;
	int m_nSkill;
	int m_nImageTexture;
	int m_nImageFor;
};

//-----------------------------------------------------------------------------
// Options: controls (remapping), game, audio, video, in tabs switched with L/R
//-----------------------------------------------------------------------------
enum
{
	TAB_CONTROLS,
	TAB_GAME,
	TAB_AUDIO,
	TAB_VIDEO,
	TAB_COUNT
};

static const char *s_pszVitaTabs[TAB_COUNT] = { "CONTROLS", "GAME", "AUDIO", "VIDEO" };

enum
{
	OPT_BIND,		// a command on a button (m_pszCvar is the command)
	OPT_CHOICE,		// a cvar with listed values ("1:Easy|2:Normal")
	OPT_SLIDER,		// a cvar between min and max
	OPT_LOOK,		// joy_yaw/pitchsensitivity together, 1-10
	OPT_CAPTIONS,	// closecaption + cc_subtitles
	OPT_FLEX,		// r_flex + r_vita_flex_dist
	OPT_RESET,		// the default controls
};

struct VitaOption_t
{
	int m_nTab;
	int m_nType;
	const char *m_pszLabel;
	const char *m_pszCvar;
	const char *m_pszChoices;
	float m_flMin, m_flMax, m_flStep;
};

static const VitaOption_t s_VitaOptions[] =
{
	{ TAB_CONTROLS, OPT_BIND, "Fire", "+attack" },
	{ TAB_CONTROLS, OPT_BIND, "Alt fire", "+attack2" },
	{ TAB_CONTROLS, OPT_BIND, "Jump", "+jump" },
	{ TAB_CONTROLS, OPT_BIND, "Crouch", "+duck" },
	{ TAB_CONTROLS, OPT_BIND, "Use", "+use" },
	{ TAB_CONTROLS, OPT_BIND, "Reload", "+reload" },
	{ TAB_CONTROLS, OPT_BIND, "Sprint", "+speed" },
	{ TAB_CONTROLS, OPT_BIND, "Flashlight", "impulse 100" },
	{ TAB_CONTROLS, OPT_BIND, "Zoom", "+zoom" },
	{ TAB_CONTROLS, OPT_BIND, "Next weapon", "invnext" },
	{ TAB_CONTROLS, OPT_BIND, "Previous weapon", "invprev" },
	{ TAB_CONTROLS, OPT_BIND, "Last weapon", "lastinv" },
	{ TAB_CONTROLS, OPT_BIND, "Gravity gun", "phys_swap" },
	{ TAB_CONTROLS, OPT_BIND, "Quick save", "save quick" },
	{ TAB_CONTROLS, OPT_BIND, "Quick load", "load quick" },
	{ TAB_CONTROLS, OPT_LOOK, "Look sensitivity" },
	{ TAB_CONTROLS, OPT_CHOICE, "Invert look", "joy_inverty", "0:Off|1:On" },
	// gyro aiming (client in_joystick.cpp; GitHub issue #3)
	{ TAB_CONTROLS, OPT_CHOICE, "Gyro aiming", "vita_gyro", "0:Off|1:On" },
	{ TAB_CONTROLS, OPT_SLIDER, "Gyro sensitivity", "vita_gyro_sensitivity", NULL, 0.5f, 3.0f, 0.25f },
	{ TAB_CONTROLS, OPT_RESET, "Restore default controls" },
	{ TAB_GAME, OPT_CHOICE, "Difficulty", "skill", "1:Easy|2:Normal|3:Hard" },
	{ TAB_GAME, OPT_CAPTIONS, "Subtitles" },
	{ TAB_GAME, OPT_CHOICE, "Crosshair", "crosshair", "0:Off|1:On" },
	{ TAB_AUDIO, OPT_SLIDER, "Volume", "volume", NULL, 0.0f, 1.0f, 0.1f },
	{ TAB_AUDIO, OPT_SLIDER, "Music", "snd_musicvolume", NULL, 0.0f, 1.0f, 0.1f },
	{ TAB_VIDEO, OPT_CHOICE, "HUD size", "vita_hud_size", "0:Normal|1:Large|2:Larger" },	// client clientmode_shared.cpp
	{ TAB_VIDEO, OPT_CHOICE, "Show FPS", "cl_showfps", "0:Off|1:On" },
	{ TAB_VIDEO, OPT_FLEX, "Facial animation" },
	// static props under this radius on screen aren't drawn (client viewrender.cpp)
	{ TAB_VIDEO, OPT_CHOICE, "Distant props", "vita_staticprop_minpixels", "0:All|3:Normal|6:Fewer" },
	{ TAB_VIDEO, OPT_CHOICE, "Fire smoke", "vita_fire_smoke", "0:Off|1:On" },	// client c_fire_smoke.cpp
};

// Portal's controls are the ones in its keyboard list (scripts/kb_act.lst):
// the portal gun's two portals are named, and HL2's weapon and suit
// actions are left out. Games without a difficulty setting (nodifficulty
// in gameinfo.txt) have no Difficulty row
static const char *VitaOptionLabel( const VitaOption_t &opt )
{
	if ( opt.m_nType == OPT_BIND && !V_stricmp( COM_GetModDirectory(), "portal" ) )
	{
		if ( !V_strcmp( opt.m_pszCvar, "+attack" ) )
			return "Blue portal";
		if ( !V_strcmp( opt.m_pszCvar, "+attack2" ) )
			return "Orange portal";
	}
	return opt.m_pszLabel;
}

static bool VitaOptionShown( const VitaOption_t &opt )
{
	if ( opt.m_nType == OPT_CHOICE && !V_strcmp( opt.m_pszCvar, "skill" ) )
		return !ModInfo().NoDifficulty();
	if ( opt.m_nType != OPT_BIND || V_stricmp( COM_GetModDirectory(), "portal" ) )
		return true;
	static const char *s_pszNotInPortal[] = { "+reload", "+speed", "impulse 100", "+zoom", "invnext", "invprev", "lastinv", "phys_swap" };
	for ( int i = 0; i < ARRAYSIZE( s_pszNotInPortal ); i++ )
		if ( !V_strcmp( opt.m_pszCvar, s_pszNotInPortal[i] ) )
			return false;
	return true;
}

// Saved to cfg/vita_settings.cfg, which runs after the built-in Vita settings
// at launch (engine/host.cpp)
static const char *s_pszVitaSavedCvars[] =
{
	"skill", "closecaption", "cc_subtitles", "crosshair", "volume", "snd_musicvolume", "cl_showfps",
	"r_flex", "r_vita_flex_dist", "joy_yawsensitivity", "joy_pitchsensitivity", "joy_inverty",
	"vita_staticprop_minpixels", "vita_hud_size", "vita_fire_smoke", "vita_gyro", "vita_gyro_sensitivity",
};

// The remappable buttons (START stays the menu button)
static const ButtonCode_t s_VitaButtons[] =
{
	KEY_XBUTTON_A, KEY_XBUTTON_B, KEY_XBUTTON_X, KEY_XBUTTON_Y,
	KEY_XBUTTON_LEFT_SHOULDER, KEY_XBUTTON_RIGHT_SHOULDER, KEY_XBUTTON_BACK,
	KEY_XBUTTON_UP, KEY_XBUTTON_DOWN, KEY_XBUTTON_LEFT, KEY_XBUTTON_RIGHT,
	KEY_XBUTTON_STICK1, KEY_XBUTTON_STICK2,
	VITA_FRONT_TOUCH_LEFT, VITA_FRONT_TOUCH_RIGHT,
};

static int VitaButtonGlyph( ButtonCode_t code )
{
	switch ( code )
	{
	case KEY_XBUTTON_A:					return VG_CROSS;
	case KEY_XBUTTON_B:					return VG_CIRCLE;
	case KEY_XBUTTON_X:					return VG_SQUARE;
	case KEY_XBUTTON_Y:					return VG_TRIANGLE;
	case KEY_XBUTTON_LEFT_SHOULDER:		return VG_L;
	case KEY_XBUTTON_RIGHT_SHOULDER:	return VG_R;
	case KEY_XBUTTON_BACK:				return VG_SELECT;
	case KEY_XBUTTON_UP:
	case KEY_XBUTTON_DOWN:
	case KEY_XBUTTON_LEFT:
	case KEY_XBUTTON_RIGHT:				return VG_DPAD;
	case KEY_XBUTTON_STICK1:
	case KEY_XBUTTON_STICK2:			return VG_REAR;
	case VITA_FRONT_TOUCH_LEFT:
	case VITA_FRONT_TOUCH_RIGHT:		return VG_SCREEN;
	default:							return VG_NONE;
	}
}

// What follows the symbol (the d-pad and each touch panel have one symbol)
static const char *VitaButtonSuffix( ButtonCode_t code )
{
	switch ( code )
	{
	case KEY_XBUTTON_UP:		return "Up";
	case KEY_XBUTTON_DOWN:		return "Down";
	case KEY_XBUTTON_LEFT:		return "Left";
	case KEY_XBUTTON_RIGHT:		return "Right";
	case KEY_XBUTTON_STICK1:	return "Left";
	case KEY_XBUTTON_STICK2:	return "Right";
	case VITA_FRONT_TOUCH_LEFT:		return "Left";
	case VITA_FRONT_TOUCH_RIGHT:	return "Right";
	default:					return "";
	}
}

static bool VitaIsRemappable( ButtonCode_t code )
{
	for ( int i = 0; i < ARRAYSIZE( s_VitaButtons ); i++ )
	{
		if ( s_VitaButtons[i] == code )
			return true;
	}
	return false;
}

static const char *VitaBinding( ButtonCode_t code )
{
	const char *pszBinding = engine->Key_BindingForKey( code );
	return pszBinding ? pszBinding : "";
}

#define VITA_LOOK_STEP 0.25f	// sensitivity per level: 5 is the 360's -1.25

class CVitaOptionsScreen : public CVitaScreen
{
	DECLARE_CLASS_SIMPLE( CVitaOptionsScreen, CVitaScreen );

public:
	CVitaOptionsScreen( Panel *pParent, int nTab ) : BaseClass( pParent, "VitaOptions", "OPTIONS" )
	{
		m_nTab = nTab;
		m_nCapture = -1;
		m_flCaptureEnd = 0.0f;
		m_bControlsChanged = false;
	}

	virtual ~CVitaOptionsScreen()
	{
		so_set_touch_capture( 0 );
	}

	virtual void Close()
	{
		SaveSettings();
		if ( m_bControlsChanged )
			SaveControls();
		BaseClass::Close();
	}

protected:
	enum { CONFIRM_RESET = 1 };

	virtual void Refresh()
	{
		m_Rows.RemoveAll();
		m_RowOption.RemoveAll();
		for ( int i = 0; i < ARRAYSIZE( s_VitaOptions ); i++ )
		{
			const VitaOption_t &opt = s_VitaOptions[i];
			if ( opt.m_nTab != m_nTab || !VitaOptionShown( opt ) )
				continue;
			VitaRow_t &row = m_Rows[m_Rows.AddToTail()];
			m_RowOption.AddToTail( i );
			row.m_Label = VitaOptionLabel( opt );
			FillValue( opt, row, m_Rows.Count() - 1 );
		}
		UpdateHints();
	}

	void UpdateHints()
	{
		m_Hints.RemoveAll();
		if ( m_nCapture >= 0 )
		{
			const VitaHint_t hint = { VG_START, "Cancel" };
			m_Hints.AddToTail( hint );
			return;
		}
		const VitaHint_t hints[] = { { VG_LR, "Tab" }, { VG_CROSS, "Change" }, { VG_CIRCLE, "Back" } };
		m_Hints.AddMultipleToTail( 3, hints );
	}

	void FillValue( const VitaOption_t &opt, VitaRow_t &row, int nRow )
	{
		row.m_Value = "";
		row.m_nGlyph = VG_NONE;
		row.m_flBar = -1.0f;
		row.m_bArrows = false;
		switch ( opt.m_nType )
		{
		case OPT_BIND:
		{
			if ( nRow == m_nCapture )
			{
				row.m_Value = "Press a button or tap the screen...";
				break;
			}
			// the first remappable button that runs it, and how many more
			int nFound = 0;
			ButtonCode_t first = BUTTON_CODE_NONE;
			for ( int b = 0; b < ARRAYSIZE( s_VitaButtons ); b++ )
			{
				if ( !V_stricmp( VitaBinding( s_VitaButtons[b] ), opt.m_pszCvar ) )
				{
					if ( !nFound++ )
						first = s_VitaButtons[b];
				}
			}
			if ( !nFound )
			{
				row.m_Value = "-";
				break;
			}
			row.m_nGlyph = VitaButtonGlyph( first );
			char szValue[32];
			V_snprintf( szValue, sizeof( szValue ), nFound > 1 ? "%s +%d" : "%s", VitaButtonSuffix( first ), nFound - 1 );
			row.m_Value = szValue;
			break;
		}
		case OPT_CHOICE:
		{
			ConVarRef var( opt.m_pszCvar );
			row.m_bArrows = true;
			row.m_Value = ChoiceLabel( opt.m_pszChoices, var.IsValid() ? var.GetInt() : 0 );
			break;
		}
		case OPT_SLIDER:
		{
			ConVarRef var( opt.m_pszCvar );
			const float flValue = var.IsValid() ? var.GetFloat() : 0.0f;
			row.m_bArrows = true;
			row.m_flBar = ( flValue - opt.m_flMin ) / ( opt.m_flMax - opt.m_flMin );
			char szValue[16];
			V_snprintf( szValue, sizeof( szValue ), "%d%%", (int)( row.m_flBar * 100.0f + 0.5f ) );
			row.m_Value = szValue;
			break;
		}
		case OPT_LOOK:
		{
			const int nLevel = LookLevel();
			row.m_bArrows = true;
			row.m_flBar = nLevel / 10.0f;
			char szValue[16];
			V_snprintf( szValue, sizeof( szValue ), "%d", nLevel );
			row.m_Value = szValue;
			break;
		}
		case OPT_CAPTIONS:
		{
			static const char *s_pszModes[] = { "Off", "Subtitles", "Captions" };
			row.m_bArrows = true;
			row.m_Value = s_pszModes[CaptionMode()];
			break;
		}
		case OPT_FLEX:
		{
			static const char *s_pszModes[] = { "Off", "Near", "Far" };
			row.m_bArrows = true;
			row.m_Value = s_pszModes[FlexMode()];
			break;
		}
		default:
			break;
		}
	}

	static const char *ChoiceLabel( const char *pszChoices, int nValue )
	{
		static char s_szLabel[32];
		s_szLabel[0] = 0;
		CUtlStringList choices;
		V_SplitString( pszChoices, "|", choices );
		for ( int i = 0; i < choices.Count(); i++ )
		{
			const char *pColon = strchr( choices[i], ':' );
			if ( pColon && atoi( choices[i] ) == nValue )
			{
				V_strncpy( s_szLabel, pColon + 1, sizeof( s_szLabel ) );
				break;
			}
		}
		return s_szLabel;
	}

	static int NextChoice( const char *pszChoices, int nValue, int nDir )
	{
		CUtlStringList choices;
		V_SplitString( pszChoices, "|", choices );
		if ( !choices.Count() )
			return nValue;
		int nIndex = 0;
		for ( int i = 0; i < choices.Count(); i++ )
		{
			if ( atoi( choices[i] ) == nValue )
				nIndex = i;
		}
		nIndex = clamp( nIndex + nDir, 0, choices.Count() - 1 );
		return atoi( choices[nIndex] );
	}

	static int LookLevel()
	{
		ConVarRef yaw( "joy_yawsensitivity" );
		const float flYaw = yaw.IsValid() ? fabsf( yaw.GetFloat() ) : 1.25f;
		return clamp( (int)( flYaw / VITA_LOOK_STEP + 0.5f ), 1, 10 );
	}

	static void SetLookLevel( int nLevel )
	{
		ConVarRef yaw( "joy_yawsensitivity" );
		ConVarRef pitch( "joy_pitchsensitivity" );
		if ( !yaw.IsValid() || !pitch.IsValid() )
			return;
		const float flYaw = yaw.GetFloat(), flPitch = pitch.GetFloat();
		// keep the signs and the pitch/yaw ratio
		const float flRatio = fabsf( flYaw ) > 0.01f ? fabsf( flPitch ) / fabsf( flYaw ) : 0.8f;
		const float flNewYaw = nLevel * VITA_LOOK_STEP;
		yaw.SetValue( flYaw > 0.0f ? flNewYaw : -flNewYaw );
		pitch.SetValue( flPitch < 0.0f ? -flNewYaw * flRatio : flNewYaw * flRatio );
	}

	static int CaptionMode()
	{
		ConVarRef cc( "closecaption" );
		ConVarRef subtitles( "cc_subtitles" );
		if ( !cc.IsValid() || !cc.GetBool() )
			return 0;
		return ( subtitles.IsValid() && subtitles.GetBool() ) ? 1 : 2;
	}

	static void SetCaptionMode( int nMode )
	{
		ConVarRef cc( "closecaption" );
		ConVarRef subtitles( "cc_subtitles" );
		if ( cc.IsValid() )
			cc.SetValue( nMode != 0 );
		if ( subtitles.IsValid() )
			subtitles.SetValue( nMode == 1 );
	}

	static int FlexMode()
	{
		ConVarRef flex( "r_flex" );
		ConVarRef dist( "r_vita_flex_dist" );
		if ( !flex.IsValid() || !flex.GetBool() )
			return 0;
		return ( dist.IsValid() && dist.GetFloat() > 384.0f ) ? 2 : 1;
	}

	static void SetFlexMode( int nMode )
	{
		ConVarRef flex( "r_flex" );
		ConVarRef dist( "r_vita_flex_dist" );
		if ( flex.IsValid() )
			flex.SetValue( nMode != 0 );
		if ( dist.IsValid() && nMode )
			dist.SetValue( nMode == 2 ? 512 : 256 );
	}

	virtual void OnAdjustRow( int nRow, int nDir )
	{
		if ( !m_RowOption.IsValidIndex( nRow ) )
			return;
		const VitaOption_t &opt = s_VitaOptions[m_RowOption[nRow]];
		switch ( opt.m_nType )
		{
		case OPT_CHOICE:
		{
			ConVarRef var( opt.m_pszCvar );
			if ( var.IsValid() )
				var.SetValue( NextChoice( opt.m_pszChoices, var.GetInt(), nDir ) );
			break;
		}
		case OPT_SLIDER:
		{
			ConVarRef var( opt.m_pszCvar );
			if ( var.IsValid() )
			{
				const float flValue = clamp( var.GetFloat() + nDir * opt.m_flStep, opt.m_flMin, opt.m_flMax );
				var.SetValue( floorf( flValue * 100.0f + 0.5f ) / 100.0f );
			}
			break;
		}
		case OPT_LOOK:
			SetLookLevel( clamp( LookLevel() + nDir, 1, 10 ) );
			break;
		case OPT_CAPTIONS:
			SetCaptionMode( clamp( CaptionMode() + nDir, 0, 2 ) );
			break;
		case OPT_FLEX:
			SetFlexMode( clamp( FlexMode() + nDir, 0, 2 ) );
			break;
		default:
			return;
		}
		FillValue( opt, m_Rows[nRow], nRow );
	}

	virtual void OnActivateRow( int nRow )
	{
		if ( !m_RowOption.IsValidIndex( nRow ) )
			return;
		const VitaOption_t &opt = s_VitaOptions[m_RowOption[nRow]];
		switch ( opt.m_nType )
		{
		case OPT_BIND:
			m_nCapture = nRow;
			m_flCaptureEnd = Plat_FloatTime() + 6.0f;
			so_set_touch_capture( 1 );
			FillValue( opt, m_Rows[nRow], nRow );
			UpdateHints();
			break;
		case OPT_RESET:
			Confirm( "Restore the default controls?", CONFIRM_RESET );
			break;
		case OPT_CHOICE:
		{
			// cross cycles through the values
			ConVarRef var( opt.m_pszCvar );
			if ( var.IsValid() )
			{
				const int nNext = NextChoice( opt.m_pszChoices, var.GetInt(), 1 );
				var.SetValue( nNext != var.GetInt() ? nNext : NextChoice( opt.m_pszChoices, -999999, 0 ) );
				FillValue( opt, m_Rows[nRow], nRow );
			}
			break;
		}
		default:
			OnAdjustRow( nRow, 1 );
			break;
		}
	}

	virtual void OnConfirmed( int nId )
	{
		if ( nId != CONFIRM_RESET )
			return;
		// the stock layout under the port's binds, without the saved layout
		g_pFullFileSystem->RemoveFile( "cfg/vita_controls.cfg", "MOD" );
		engine->ExecuteClientCmd( "exec 360controller.cfg\nvita_binds\n" );
		m_bControlsChanged = false;
		Refresh();
	}

	// the next button pressed after cross on a control row
	virtual bool OnCaptureKey( KeyCode code )
	{
		if ( m_nCapture < 0 )
			return false;
		const int nRow = m_nCapture;
		if ( code == KEY_XBUTTON_START || code == KEY_ESCAPE )
		{
			EndCapture();
			return true;
		}
		if ( !VitaIsRemappable( code ) || !m_RowOption.IsValidIndex( nRow ) )
			return true;	// ignored while waiting
		Bind( s_VitaOptions[m_RowOption[nRow]].m_pszCvar, code );
		VitaPlaySound( "UI/buttonclick.wav" );
		EndCapture();
		return true;
	}

	void EndCapture()
	{
		const int nRow = m_nCapture;
		m_nCapture = -1;
		so_set_touch_capture( 0 );
		if ( m_RowOption.IsValidIndex( nRow ) )
			FillValue( s_VitaOptions[m_RowOption[nRow]], m_Rows[nRow], nRow );
		// other rows may have lost their button to this one
		Refresh();
	}

	// Puts the command on the button; the button's old command takes the
	// command's old button, so nothing ends up without one
	void Bind( const char *pszCommand, ButtonCode_t code )
	{
		const CUtlString previous = VitaBinding( code );
		if ( !V_stricmp( previous.Get(), pszCommand ) )
			return;
		CUtlVector< ButtonCode_t > oldButtons;
		for ( int b = 0; b < ARRAYSIZE( s_VitaButtons ); b++ )
		{
			if ( s_VitaButtons[b] != code && !V_stricmp( VitaBinding( s_VitaButtons[b] ), pszCommand ) )
				oldButtons.AddToTail( s_VitaButtons[b] );
		}
		char szCmd[256];
		V_snprintf( szCmd, sizeof( szCmd ), "bind \"%s\" \"%s\"\n", g_pInputSystem->ButtonCodeToString( code ), pszCommand );
		engine->ExecuteClientCmd( szCmd );
		for ( int i = 0; i < oldButtons.Count(); i++ )
		{
			if ( i == 0 && !previous.IsEmpty() )
				V_snprintf( szCmd, sizeof( szCmd ), "bind \"%s\" \"%s\"\n", g_pInputSystem->ButtonCodeToString( oldButtons[i] ), previous.Get() );
			else
				V_snprintf( szCmd, sizeof( szCmd ), "unbind \"%s\"\n", g_pInputSystem->ButtonCodeToString( oldButtons[i] ) );
			engine->ExecuteClientCmd( szCmd );
		}
		m_bControlsChanged = true;
	}

	virtual void OnThink()
	{
		if ( m_nCapture >= 0 && Plat_FloatTime() > m_flCaptureEnd )
			EndCapture();
		BaseClass::OnThink();
	}

	// While waiting for a button, a tap is a touchscreen half being assigned
	// (inputsystem posts it as a button), not a tap on a row
	virtual void OnMousePressed( MouseCode code )
	{
		if ( m_nCapture >= 0 )
			return;
		BaseClass::OnMousePressed( code );
	}

	virtual void OnBack()
	{
		if ( m_nCapture >= 0 )
			return;		// circle may be the button being assigned
		BaseClass::OnBack();
	}

	virtual void OnShoulder( int nDir )
	{
		SetTab( ( m_nTab + nDir + TAB_COUNT ) % TAB_COUNT );
	}

	void SetTab( int nTab )
	{
		if ( nTab == m_nTab || m_nCapture >= 0 )
			return;
		VitaPlaySound( "UI/buttonrollover.wav" );
		m_nTab = nTab;
		m_nScroll = 0;
		Refresh();
		m_nSel = -1;
		SetSelection( FirstEnabledRow() );
	}

	virtual int HeaderTall() const
	{
		return surface()->GetFontTall( VitaFont( VITA_FONT_SMALL ) ) + VS( 10 );
	}

	void GetTabRect( int nTab, int &x, int &wide ) const
	{
		int sw, st;
		GetSize( sw, st );
		const int margin = VS( 28 );
		const int tabWide = ( sw - 2 * margin ) / TAB_COUNT;
		x = margin + nTab * tabWide;
		wide = tabWide;
	}

	virtual void PaintHeader( int x, int y, int wide )
	{
		HFont small = VitaFont( VITA_FONT_SMALL );
		const int fontTall = surface()->GetFontTall( small );
		for ( int i = 0; i < TAB_COUNT; i++ )
		{
			int tx, tw;
			GetTabRect( i, tx, tw );
			const bool bSel = ( i == m_nTab );
			VitaDrawText( small, tx + tw / 2, y, s_pszVitaTabs[i], bSel ? Color( 255, 255, 255, 255 ) : Color( 150, 150, 150, 255 ), 0 );
			if ( bSel )
			{
				surface()->DrawSetColor( 255, 176, 0, 255 );
				surface()->DrawFilledRect( tx + tw / 4, y + fontTall + VS( 2 ), tx + tw * 3 / 4, y + fontTall + VS( 4 ) );
			}
		}
	}

	virtual void OnTapHeader( int x )
	{
		for ( int i = 0; i < TAB_COUNT; i++ )
		{
			int tx, tw;
			GetTabRect( i, tx, tw );
			if ( x >= tx && x < tx + tw )
				SetTab( i );
		}
	}

private:
	void SaveSettings()
	{
		FileHandle_t f = g_pFullFileSystem->Open( "cfg/vita_settings.cfg", "w", "MOD" );
		if ( f == FILESYSTEM_INVALID_HANDLE )
			return;
		g_pFullFileSystem->FPrintf( f, "// Written by the Vita options menu; runs after the port's built-in settings at launch\n" );
		for ( int i = 0; i < ARRAYSIZE( s_pszVitaSavedCvars ); i++ )
		{
			ConVarRef var( s_pszVitaSavedCvars[i] );
			if ( var.IsValid() )
				g_pFullFileSystem->FPrintf( f, "%s \"%s\"\n", s_pszVitaSavedCvars[i], var.GetString() );
		}
		g_pFullFileSystem->Close( f );
	}

	// every remappable button's command; vita_binds runs it after the defaults
	void SaveControls()
	{
		FileHandle_t f = g_pFullFileSystem->Open( "cfg/vita_controls.cfg", "w", "MOD" );
		if ( f == FILESYSTEM_INVALID_HANDLE )
			return;
		g_pFullFileSystem->FPrintf( f, "// Written by the Vita options menu (Controls); runs after the default controls\n" );
		for ( int b = 0; b < ARRAYSIZE( s_VitaButtons ); b++ )
		{
			const char *pszName = g_pInputSystem->ButtonCodeToString( s_VitaButtons[b] );
			const char *pszBinding = VitaBinding( s_VitaButtons[b] );
			if ( pszBinding[0] && !strchr( pszBinding, '"' ) )
				g_pFullFileSystem->FPrintf( f, "bind \"%s\" \"%s\"\n", pszName, pszBinding );
			else if ( !pszBinding[0] )
				g_pFullFileSystem->FPrintf( f, "unbind \"%s\"\n", pszName );
		}
		g_pFullFileSystem->Close( f );
	}

	CUtlVector< int > m_RowOption;	// row -> s_VitaOptions index
	int m_nTab;
	int m_nCapture;					// row waiting for a button, -1 none
	float m_flCaptureEnd;
	bool m_bControlsChanged;
};

//-----------------------------------------------------------------------------
// Quit: back to the main menu (in a game) or out of the game
//-----------------------------------------------------------------------------
class CVitaQuitScreen : public CVitaScreen
{
	DECLARE_CLASS_SIMPLE( CVitaQuitScreen, CVitaScreen );

public:
	CVitaQuitScreen( Panel *pParent ) : BaseClass( pParent, "VitaQuit", "QUIT" ) {}

protected:
	enum { ROW_MAINMENU, ROW_QUIT, ROW_CANCEL };

	virtual void Refresh()
	{
		m_Rows.RemoveAll();
		m_RowAction.RemoveAll();
		if ( GameUI().IsInLevel() )
			AddRow( "Return to the main menu", ROW_MAINMENU );
		AddRow( !V_stricmp( COM_GetModDirectory(), "portal" ) ? "Quit Portal" : "Quit Half-Life 2", ROW_QUIT );
		AddRow( "Cancel", ROW_CANCEL );
		m_Hints.RemoveAll();
		const VitaHint_t hints[] = { { VG_CROSS, "Select" }, { VG_CIRCLE, "Back" } };
		m_Hints.AddMultipleToTail( 2, hints );
	}

	void AddRow( const char *pszLabel, int nAction )
	{
		VitaRow_t &row = m_Rows[m_Rows.AddToTail()];
		row.m_Label = pszLabel;
		m_RowAction.AddToTail( nAction );
	}

	virtual void OnActivateRow( int nRow )
	{
		if ( !m_RowAction.IsValidIndex( nRow ) )
			return;
		switch ( m_RowAction[nRow] )
		{
		case ROW_MAINMENU:
			Close();
			engine->ClientCmd_Unrestricted( "disconnect\n" );
			break;
		case ROW_QUIT:
			CloseQuietly();
			PostMessage( BasePanel(), new KeyValues( "Command", "command", "QuitNoConfirm" ) );
			break;
		default:
			Close();
			break;
		}
	}

private:
	CUtlVector< int > m_RowAction;
};

//-----------------------------------------------------------------------------
// Entry points (CBasePanel::RunMenuCommand)
//-----------------------------------------------------------------------------
void VitaMenu_OpenNewGame( Panel *pParent )
{
	( new CVitaNewGameScreen( pParent ) )->Open();
}

void VitaMenu_OpenLoadGame( Panel *pParent )
{
	( new CVitaSaveLoadScreen( pParent, false ) )->Open();
}

void VitaMenu_OpenSaveGame( Panel *pParent )
{
	( new CVitaSaveLoadScreen( pParent, true ) )->Open();
}

void VitaMenu_OpenOptions( Panel *pParent )
{
	( new CVitaOptionsScreen( pParent, TAB_CONTROLS ) )->Open();
}

void VitaMenu_OpenQuit( Panel *pParent )
{
	( new CVitaQuitScreen( pParent ) )->Open();
}
