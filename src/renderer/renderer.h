#pragma once
#include <pch.h>
#include "renderer/glyph_renderer.h"

constexpr const char *DEFAULT_FONT = "Consolas";
constexpr float DEFAULT_FONT_SIZE = 14.0f;

constexpr uint32_t DEFAULT_COLOR = 0x46464646;
enum HighlightAttributeFlags : uint16_t {
	HL_ATTRIB_REVERSE			= 1 << 0,
	HL_ATTRIB_ITALIC			= 1 << 1,
	HL_ATTRIB_BOLD				= 1 << 2,
	HL_ATTRIB_STRIKETHROUGH		= 1 << 3,
	HL_ATTRIB_UNDERLINE			= 1 << 4,
	HL_ATTRIB_UNDERCURL			= 1 << 5
};
struct HighlightAttributes {
	uint32_t foreground;
	uint32_t background;
	uint32_t special;
	uint16_t flags;
};

enum class CursorShape {
	None,
	Block,
	Vertical,
	Horizontal
};

struct GridPoint {
	int row;
	int col;
};
struct GridSize {
	int rows;
	int cols;
};
struct PixelSize {
	int width;
	int height;
};

struct CursorModeInfo {
	CursorShape shape;
	uint16_t hl_attrib_id;
};
struct Cursor {
	CursorModeInfo *mode_info;
	int row;
	int col;
};

struct CellProperty {
	uint16_t hl_attrib_id;
	bool is_wide_char;
};

constexpr int MAX_HIGHLIGHT_ATTRIBS = 0xFFFF;
constexpr int MAX_CURSOR_MODE_INFOS = 64;
constexpr int MAX_FONT_LENGTH = 128;
constexpr float DEFAULT_DPI = 96.0f;
constexpr float POINTS_PER_INCH = 72.0f;
struct GlyphDrawingEffect;
struct GlyphRenderer;
using Microsoft::WRL::ComPtr;
struct Renderer {
	CursorModeInfo cursor_mode_infos[MAX_CURSOR_MODE_INFOS];
	Vec<HighlightAttributes> hl_attribs;
	Cursor cursor;

	std::unique_ptr<GlyphRenderer> glyph_renderer;

	D3D_FEATURE_LEVEL d3d_feature_level;
	ComPtr<ID3D11Device2> d3d_device;
	ComPtr<ID3D11DeviceContext2> d3d_context;
	ComPtr<IDXGISwapChain2> dxgi_swapchain;
	HANDLE swapchain_wait_handle;
	ComPtr<ID2D1Factory5> d2d_factory;
	ComPtr<ID2D1Device4> d2d_device;
	ComPtr<ID2D1DeviceContext4> d2d_context;
	ComPtr<ID2D1Bitmap1> d2d_target_bitmap;
	ComPtr<ID2D1SolidColorBrush> d2d_background_rect_brush;

    ComPtr<IDWriteFontFace1> font_face;

	ComPtr<IDWriteFactory4> dwrite_factory;
	ComPtr<IDWriteTextFormat> dwrite_text_format;

	bool disable_ligatures;
	ComPtr<IDWriteTypography> dwrite_typography;

	float linespace_factor;

    float last_requested_font_size;
	wchar_t font[MAX_FONT_LENGTH];
	wchar_t fallback_font[MAX_FONT_LENGTH];
	DWRITE_FONT_METRICS1 font_metrics;
	float font_size_scale_bold;
	float dpi_scale;
    float font_size;
	float font_height;
	float font_width;
	float font_ascent;
    float font_descent;

	D2D1_SIZE_U pixel_size;
	bool grid_initialized;
	int grid_rows;
	int grid_cols;
	std::unique_ptr<uint32_t[]> grid_chars;
	std::unique_ptr<wchar_t[]> wchar_buffer;
	size_t wchar_buffer_length;
	std::unique_ptr<CellProperty[]> grid_cell_properties;

	HWND hwnd;
	bool draw_active;
	bool ui_busy;
	bool has_drawn;
	bool draws_invalidated;
};

void RendererInitialize(Renderer *renderer, HWND hwnd, bool disable_ligatures, float linespace_factor, float monitor_dpi);
void RendererAttach(Renderer *renderer);
void RendererShutdown(Renderer *renderer);

void RendererResize(Renderer *renderer, uint32_t width, uint32_t height);
bool RendererUpdateGuiFont(Renderer *renderer, const char *guifont, size_t strlen);
bool RendererUpdateFont(Renderer *renderer, float font_size, const char *font_string = "", int strlen = 0);
void RendererRedraw(Renderer *renderer, mpack_node_t params, bool start_maximized);
void RendererFlush(Renderer* renderer);

PixelSize RendererGridToPixelSize(Renderer *renderer, int rows, int cols);
GridSize RendererPixelsToGridSize(Renderer *renderer, int width, int height);
GridPoint RendererCursorToGridPoint(Renderer *renderer, int x, int y);
