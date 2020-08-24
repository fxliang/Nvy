#include "pch.h"
#include "renderer.h"

#include "common/mpack_helper.h"
#include "renderer/glyph_renderer.h"

#define WIN_CHECK(x) { \
HRESULT ret = x; \
if(ret != S_OK) printf("HRESULT: %s is 0x%X in %s at line %d\n", #x, x, __FILE__, __LINE__); \
}

void RendererInitialize(Renderer *renderer, HWND hwnd, const char *font, float font_size) {
	renderer->hwnd = hwnd;
	renderer->dpi_scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;

	WIN_CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &renderer->d2d_factory));

	D2D1_RENDER_TARGET_PROPERTIES target_props = D2D1_RENDER_TARGET_PROPERTIES {
		.type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
		.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
		.dpiX = 96.0f,
		.dpiY = 96.0f,
	};

	RECT client_rect;
	GetClientRect(hwnd, &client_rect);
	renderer->pixel_size = D2D1_SIZE_U {
		.width = static_cast<uint32_t>(client_rect.right - client_rect.left),
		.height = static_cast<uint32_t>(client_rect.bottom - client_rect.top)
	};
	D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props = D2D1_HWND_RENDER_TARGET_PROPERTIES {
		.hwnd = hwnd,
		.pixelSize = renderer->pixel_size,
		.presentOptions = D2D1_PRESENT_OPTIONS_IMMEDIATELY
	};

	WIN_CHECK(renderer->d2d_factory->CreateHwndRenderTarget(target_props, hwnd_props, &renderer->render_target));
	renderer->render_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
	renderer->render_target->CreateBitmap(
		renderer->pixel_size,
		D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
		&renderer->scroll_region_bitmap
	);

	WIN_CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(&renderer->write_factory)));

	renderer->glyph_renderer = new GlyphRenderer;
	RendererUpdateFont(renderer, font_size, font, static_cast<int>(strlen(font)));
}

void RendererShutdown(Renderer *renderer) {
	delete renderer->glyph_renderer;

	renderer->scroll_region_bitmap->Release();
	renderer->d2d_factory->Release();
	renderer->render_target->Release();
	renderer->write_factory->Release();
	renderer->text_format->Release();

	free(renderer->grid_chars);
	free(renderer->grid_cell_properties);
}

void RendererResize(Renderer *renderer, uint32_t width, uint32_t height) {
	renderer->pixel_size.width = width;
	renderer->pixel_size.height = height;
	WIN_CHECK(renderer->render_target->Resize(&renderer->pixel_size));
	renderer->scroll_region_bitmap->Release();
	renderer->render_target->CreateBitmap(
		renderer->pixel_size,
		D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
		&renderer->scroll_region_bitmap
	);
}

float GetCharacterWidth(Renderer *renderer, wchar_t wchar) {
	// Create dummy text format to hit test the width of the font
	IDWriteTextLayout *test_text_layout = nullptr;
	WIN_CHECK(renderer->write_factory->CreateTextLayout(
		&wchar,
		1,
		renderer->text_format,
		0.0f,
		0.0f,
		&test_text_layout
	));

	DWRITE_HIT_TEST_METRICS metrics;
	float _;
	WIN_CHECK(test_text_layout->HitTestTextPosition(0, 0, &_, &_, &metrics));
	test_text_layout->Release();

	return metrics.width;
}

void UpdateFontSize(Renderer *renderer, float font_size) {
	renderer->font_size = font_size * renderer->dpi_scale;

	WIN_CHECK(renderer->write_factory->CreateTextFormat(
		renderer->font,
		nullptr,
		DWRITE_FONT_WEIGHT_NORMAL,
		DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL,
		renderer->font_size,
		L"en-us",
		&renderer->text_format
	));

	// Update the width based on a hit test
	renderer->font_width = GetCharacterWidth(renderer, L'A');

	// We calculate the height and the baseline from the font metrics, 
	// and ensure uniform line spacing. This ensures that double-width
	// characters and characters using a fallback font stay on the line
	int font_height_em = renderer->font_metrics.ascent + renderer->font_metrics.descent + renderer->font_metrics.lineGap;
	renderer->font_height = (static_cast<float>(font_height_em) * renderer->font_size) /
		static_cast<float>(renderer->font_metrics.designUnitsPerEm);

	WIN_CHECK(renderer->text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));
	WIN_CHECK(renderer->text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));

	float baseline = (static_cast<float>(renderer->font_metrics.ascent) * renderer->font_size) /
		static_cast<float>(renderer->font_metrics.designUnitsPerEm);

	renderer->line_spacing = ceilf(renderer->font_height);
	WIN_CHECK(renderer->text_format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, renderer->line_spacing, baseline));
}

void UpdateFontMetrics(Renderer *renderer, const char* font_string, int strlen) {
	if (strlen == 0) {
		return;
	}

	IDWriteFontCollection *font_collection;
	WIN_CHECK(renderer->write_factory->GetSystemFontCollection(&font_collection));

	int wstrlen = MultiByteToWideChar(CP_UTF8, 0, font_string, strlen, 0, 0);

	if (wstrlen < MAX_FONT_LENGTH) {
		MultiByteToWideChar(CP_UTF8, 0, font_string, strlen, renderer->font, MAX_FONT_LENGTH - 1);
		renderer->font[wstrlen] = L'\0';
	}

	uint32_t index;
	BOOL exists;
	font_collection->FindFamilyName(renderer->font, &index, &exists);

	// Fallback font
	if (!exists) {
		font_collection->FindFamilyName(L"Consolas", &index, &exists);
	}

	IDWriteFontFamily *font_family;
	font_collection->GetFontFamily(index, &font_family);

	IDWriteFont *write_font;
	font_family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &write_font);

	IDWriteFont1 *write_font_1;
	write_font->QueryInterface<IDWriteFont1>(&write_font_1);
	write_font_1->GetMetrics(&renderer->font_metrics);

	write_font_1->Release();
	write_font->Release();
	font_family->Release();
	font_collection->Release();
}

void RendererUpdateFont(Renderer *renderer, float font_size, const char *font_string, int strlen) {
	if (font_size > 100.0f || font_size < 5) {
		return;
	}

	if (renderer->text_format) {
		renderer->text_format->Release();
	}
	UpdateFontMetrics(renderer, font_string, strlen);
	UpdateFontSize(renderer, font_size);
}

void UpdateDefaultColors(Renderer *renderer, mpack_node_t default_colors) {
	size_t default_colors_arr_length = mpack_node_array_length(default_colors);

	for (size_t i = 1; i < default_colors_arr_length; ++i) {
		mpack_node_t color_arr = mpack_node_array_at(default_colors, i);

		// Default colors occupy the first index of the highlight attribs array
		renderer->hl_attribs[0].foreground = static_cast<uint32_t>(mpack_node_array_at(color_arr, 0).data->value.u);
		renderer->hl_attribs[0].background = static_cast<uint32_t>(mpack_node_array_at(color_arr, 1).data->value.u);
		renderer->hl_attribs[0].special = static_cast<uint32_t>(mpack_node_array_at(color_arr, 2).data->value.u);
		renderer->hl_attribs[0].flags = 0;
	}
}

void UpdateHighlightAttributes(Renderer *renderer, mpack_node_t highlight_attribs) {
	uint64_t attrib_count = mpack_node_array_length(highlight_attribs);
	for (uint64_t i = 1; i < attrib_count; ++i) {
		int64_t attrib_index = mpack_node_array_at(mpack_node_array_at(highlight_attribs, i), 0).data->value.i;
		assert(attrib_index <= MAX_HIGHLIGHT_ATTRIBS);

		mpack_node_t attrib_map = mpack_node_array_at(mpack_node_array_at(highlight_attribs, i), 1);

		const auto SetColor = [&](const char *name, uint32_t *color) {
			mpack_node_t color_node = mpack_node_map_cstr_optional(attrib_map, name);
			if (!mpack_node_is_missing(color_node)) {
				*color = static_cast<uint32_t>(color_node.data->value.u);
			}
			else {
				*color = DEFAULT_COLOR;
			}
		};
		SetColor("foreground", &renderer->hl_attribs[attrib_index].foreground);
		SetColor("background", &renderer->hl_attribs[attrib_index].background);
		SetColor("special", &renderer->hl_attribs[attrib_index].special);

		const auto SetFlag = [&](const char *flag_name, HighlightAttributeFlags flag) {
			mpack_node_t flag_node = mpack_node_map_cstr_optional(attrib_map, flag_name);
			if (!mpack_node_is_missing(flag_node)) {
				if (flag_node.data->value.b) {
					renderer->hl_attribs[attrib_index].flags |= flag;
				}
				else {
					renderer->hl_attribs[attrib_index].flags &= ~flag;
				}
			}
		};
		SetFlag("reverse", HL_ATTRIB_REVERSE);
		SetFlag("italic", HL_ATTRIB_ITALIC);
		SetFlag("bold", HL_ATTRIB_BOLD);
		SetFlag("strikethrough", HL_ATTRIB_STRIKETHROUGH);
		SetFlag("underline", HL_ATTRIB_UNDERLINE);
		SetFlag("undercurl", HL_ATTRIB_UNDERCURL);
	}
}

uint32_t CreateForegroundColor(Renderer *renderer, HighlightAttributes *hl_attribs) {
	if (hl_attribs->flags & HL_ATTRIB_REVERSE) {
		return hl_attribs->background == DEFAULT_COLOR ? renderer->hl_attribs[0].background : hl_attribs->background;
	}
	else {
		return hl_attribs->foreground == DEFAULT_COLOR ? renderer->hl_attribs[0].foreground : hl_attribs->foreground;
	}
}

uint32_t CreateBackgroundColor(Renderer *renderer, HighlightAttributes *hl_attribs) {
	if (hl_attribs->flags & HL_ATTRIB_REVERSE) {
		return hl_attribs->foreground == DEFAULT_COLOR ? renderer->hl_attribs[0].foreground : hl_attribs->foreground;
	}
	else {
		return hl_attribs->background == DEFAULT_COLOR ? renderer->hl_attribs[0].background : hl_attribs->background;
	}
}

void ApplyHighlightAttributes(Renderer *renderer, HighlightAttributes *hl_attribs,
	IDWriteTextLayout *text_layout, int start, int end) {
	GlyphDrawingEffect *drawing_effect = new GlyphDrawingEffect(CreateForegroundColor(renderer, hl_attribs));
	DWRITE_TEXT_RANGE range {
		.startPosition = static_cast<uint32_t>(start),
		.length = static_cast<uint32_t>(end - start)
	};
	if (hl_attribs->flags & HL_ATTRIB_ITALIC) {
		text_layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
	}
	if (hl_attribs->flags & HL_ATTRIB_BOLD) {
		text_layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
	}
	if (hl_attribs->flags & HL_ATTRIB_STRIKETHROUGH) {
		text_layout->SetStrikethrough(true, range);
	}
	if (hl_attribs->flags & HL_ATTRIB_UNDERLINE) {
		text_layout->SetUnderline(true, range);
	}
	if (hl_attribs->flags & HL_ATTRIB_UNDERCURL) {
		text_layout->SetUnderline(true, range);
		drawing_effect->undercurl = true;
	}
	text_layout->SetDrawingEffect(drawing_effect, range);
}

void DrawBackgroundRect(Renderer *renderer, D2D1_RECT_F rect, HighlightAttributes *hl_attribs) {
	ID2D1SolidColorBrush *brush;
	uint32_t color = CreateBackgroundColor(renderer, hl_attribs);
	WIN_CHECK(renderer->render_target->CreateSolidColorBrush(D2D1::ColorF(color), &brush));

	renderer->render_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
	renderer->render_target->FillRectangle(&rect, brush);
	renderer->render_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

	brush->Release();
}

D2D1_RECT_F GetCursorForegroundRect(Renderer *renderer, D2D1_RECT_F cursor_bg_rect) {
	if (renderer->cursor.mode_info) {
		switch (renderer->cursor.mode_info->shape) {
		case CursorShape::None: {
		} return cursor_bg_rect;
		case CursorShape::Block: {
		} return cursor_bg_rect;
		case CursorShape::Vertical: {
			cursor_bg_rect.right = cursor_bg_rect.left + 2;
		} return cursor_bg_rect;
		case CursorShape::Horizontal: {
			cursor_bg_rect.top = cursor_bg_rect.bottom - 2;
		} return cursor_bg_rect;
		}
	}
	return cursor_bg_rect;
}

void DrawSingleCharacter(Renderer *renderer, D2D1_RECT_F rect, wchar_t character, HighlightAttributes *hl_attribs) {
	IDWriteTextLayout *text_layout = nullptr;
	WIN_CHECK(renderer->write_factory->CreateTextLayout(
		&character,
		1,
		renderer->text_format,
		rect.right - rect.left,
		rect.bottom - rect.top,
		&text_layout
	));
	ApplyHighlightAttributes(renderer, hl_attribs, text_layout, 0, 1);
	
	text_layout->Draw(renderer, renderer->glyph_renderer, rect.left, rect.top);
	text_layout->Release();
}

void EraseCursor(Renderer *renderer) {
	int cursor_grid_offset = renderer->cursor.row * renderer->grid_cols + renderer->cursor.col;
	wchar_t character_under_cursor = renderer->grid_chars[cursor_grid_offset];

	// If the cursor is outside the bounds of the client, skip erase
	if (cursor_grid_offset > renderer->grid_rows * renderer->grid_cols ||
		renderer->grid_chars[cursor_grid_offset] == L'\0') {
		return;
	}

	int double_width_char_factor = 1;
	if (cursor_grid_offset < (renderer->grid_rows *renderer->grid_cols) &&
		renderer->grid_cell_properties[cursor_grid_offset].is_wide_char) {
		double_width_char_factor += 1;
	}

	D2D1_RECT_F cursor_rect {
		.left = renderer->cursor.col * renderer->font_width,
		.top = renderer->cursor.row * renderer->line_spacing,
		.right = renderer->cursor.col * renderer->font_width + renderer->font_width * double_width_char_factor,
		.bottom = (renderer->cursor.row * renderer->line_spacing) + renderer->line_spacing
	};

	HighlightAttributes *hl_attribs = &renderer->hl_attribs[renderer->grid_cell_properties[cursor_grid_offset].hl_attrib_id];
	DrawBackgroundRect(renderer, cursor_rect, hl_attribs);
	DrawSingleCharacter(renderer, cursor_rect, character_under_cursor, hl_attribs);
}

void DrawCursor(Renderer *renderer) {
	int cursor_grid_offset = renderer->cursor.row * renderer->grid_cols + renderer->cursor.col;

	wchar_t character_under_cursor = renderer->grid_chars[cursor_grid_offset];
	int double_width_char_factor = 1;
	if (cursor_grid_offset < (renderer->grid_rows *renderer->grid_cols) && 
		renderer->grid_cell_properties[cursor_grid_offset].is_wide_char) {
		double_width_char_factor += 1;
	}
		
	HighlightAttributes cursor_hl_attribs = renderer->hl_attribs[renderer->cursor.mode_info->hl_attrib_index];
	if (renderer->cursor.mode_info->hl_attrib_index == 0) {
		cursor_hl_attribs.flags ^= HL_ATTRIB_REVERSE;
	}

	D2D1_RECT_F cursor_rect {
		.left = renderer->cursor.col * renderer->font_width,
		.top = renderer->cursor.row * renderer->line_spacing,
		.right = renderer->cursor.col * renderer->font_width + renderer->font_width * double_width_char_factor,
		.bottom = (renderer->cursor.row * renderer->line_spacing) + renderer->line_spacing
	};
	D2D1_RECT_F cursor_fg_rect = GetCursorForegroundRect(renderer, cursor_rect);
	DrawBackgroundRect(renderer, cursor_fg_rect, &cursor_hl_attribs);

	if (renderer->cursor.mode_info->shape == CursorShape::Block) {
		DrawSingleCharacter(renderer, cursor_fg_rect, character_under_cursor, &cursor_hl_attribs);
	}
}

void DrawGridLine(Renderer *renderer, int row, int start, int end) {
	int base = row * renderer->grid_cols + start;

	D2D1_RECT_F rect {
		.left = start * renderer->font_width,
		.top = row * renderer->line_spacing,
		.right = end * renderer->font_width,
		.bottom = (row * renderer->line_spacing) + renderer->line_spacing
	};

	IDWriteTextLayout *temp_text_layout = nullptr;
	WIN_CHECK(renderer->write_factory->CreateTextLayout(
		&renderer->grid_chars[base],
		end - start,
		renderer->text_format,
		rect.right - rect.left,
		rect.bottom - rect.top,
		&temp_text_layout
	));
	IDWriteTextLayout1 *text_layout;
	temp_text_layout->QueryInterface<IDWriteTextLayout1>(&text_layout);
	temp_text_layout->Release();

	uint8_t hl_attrib_id = renderer->grid_cell_properties[base].hl_attrib_id;
	int col_offset = 0;
	for (int i = 0; i < (end - start); ++i) {
		float char_width = renderer->font_width;
		bool double_width_char_next = false;
		bool cursor_next = (row == renderer->cursor.row && i == renderer->cursor.col && renderer->cursor.mode_info);

		if (i < (end - start - 1) && renderer->grid_chars[base + i + 1] == L'\0') {
			char_width = GetCharacterWidth(renderer, renderer->grid_chars[base + i]);
			DWRITE_TEXT_RANGE range { .startPosition = static_cast<uint32_t>(i), .length = 1 };
			text_layout->SetCharacterSpacing(0, (renderer->font_width * 2) - char_width, 0, range);
		}

		// Check if the attributes change, 
		// if so draw until this point and continue with the new attributes
		if (renderer->grid_cell_properties[base + i].hl_attrib_id != hl_attrib_id) {
			D2D1_RECT_F bg_rect {
				.left = (start + col_offset) * renderer->font_width,
				.top = row * renderer->line_spacing,
				.right = (start + col_offset) * renderer->font_width + renderer->font_width * (i - col_offset),
				.bottom = (row * renderer->line_spacing) + renderer->line_spacing
			};
			DrawBackgroundRect(renderer, bg_rect, &renderer->hl_attribs[hl_attrib_id]);
			ApplyHighlightAttributes(renderer, &renderer->hl_attribs[hl_attrib_id], text_layout, col_offset, i);

			hl_attrib_id = renderer->grid_cell_properties[base + i].hl_attrib_id;
			col_offset = i;
		}
	}
	
	// Draw the remaining columns, there is always atleast the last column to draw,
	// but potentially more in case the last X columns share the same hl_attrib
	rect.left = (start + col_offset) * renderer->font_width;
	DrawBackgroundRect(renderer, rect, &renderer->hl_attribs[hl_attrib_id]);
	ApplyHighlightAttributes(renderer, &renderer->hl_attribs[hl_attrib_id], text_layout, col_offset, end);

	text_layout->Draw(renderer, renderer->glyph_renderer, start * renderer->font_width, rect.top);
	text_layout->Release();
}

void DrawGridLines(Renderer *renderer, mpack_node_t grid_lines) {
	assert(renderer->grid_chars != nullptr);
	assert(renderer->grid_cell_properties != nullptr);

	size_t line_count = mpack_node_array_length(grid_lines);
	for (size_t i = 1; i < line_count; ++i) {
		mpack_node_t grid_line = mpack_node_array_at(grid_lines, i);

		int row = MPackIntFromArray(grid_line, 1);
		int col_start = MPackIntFromArray(grid_line, 2);


		mpack_node_t cells_array = mpack_node_array_at(grid_line, 3);
		size_t cells_array_length = mpack_node_array_length(cells_array);

		int col_offset = col_start;
		int hl_attrib_id = 0;
		for (size_t j = 0; j < cells_array_length; ++j) {
			mpack_node_t cells = mpack_node_array_at(cells_array, j);
			size_t cells_length = mpack_node_array_length(cells);

			mpack_node_t text = mpack_node_array_at(cells, 0);
			const char *str = mpack_node_str(text);
			int strlen = static_cast<int>(mpack_node_strlen(text));

			// Right part of double-width char is the empty string
			// skip the conversion, since DirectWrite ignores embedded
			// null bytes, we can simply insert one and go to the next column
			if (strlen == 0) {
				int offset = row * renderer->grid_cols + col_offset;
				renderer->grid_cell_properties[offset - 1].is_wide_char = true;

				renderer->grid_chars[offset] = L'\0';
				renderer->grid_cell_properties[offset].hl_attrib_id = hl_attrib_id;
				renderer->grid_cell_properties[offset].is_wide_char = true;
				col_offset += 1;
				continue;
			}

			if (cells_length > 1) {
				hl_attrib_id = MPackIntFromArray(cells, 1);
			}

			int repeat = 1;
			if (cells_length > 2) {
				repeat = MPackIntFromArray(cells, 2);
			}

			int offset = row * renderer->grid_cols + col_offset;
			int wstrlen = 0;
			for (int k = 0; k < repeat; ++k) {
				int idx = offset + (k * wstrlen);
				wstrlen = MultiByteToWideChar(CP_UTF8, 0, str, strlen, &renderer->grid_chars[idx], (renderer->grid_cols * renderer->grid_rows) - idx);
			}

			int wstrlen_with_repetitions = wstrlen * repeat;
			for (int k = 0; k < wstrlen_with_repetitions; ++k) {
				renderer->grid_cell_properties[offset + k].hl_attrib_id = hl_attrib_id;
			}

			col_offset += wstrlen_with_repetitions;
		}

		DrawGridLine(renderer, row, col_start, col_offset);
	}
}

void UpdateGridSize(Renderer *renderer, mpack_node_t grid_resize) {
	mpack_node_t grid_resize_params = mpack_node_array_at(grid_resize, 1);
	int grid_cols = MPackIntFromArray(grid_resize_params, 1);
	int grid_rows = MPackIntFromArray(grid_resize_params, 2);

	if (renderer->grid_chars == nullptr ||
		renderer->grid_cell_properties == nullptr ||
		renderer->grid_cols != grid_cols ||
		renderer->grid_rows != grid_rows) {
		
		renderer->grid_cols = grid_cols;
		renderer->grid_rows = grid_rows;

		renderer->grid_chars = static_cast<wchar_t *>(calloc(static_cast<size_t>(grid_cols) * grid_rows, sizeof(wchar_t)));
		renderer->grid_cell_properties = static_cast<CellProperty *>(calloc(static_cast<size_t>(grid_cols) * grid_rows, sizeof(CellProperty)));
	}
}

void UpdateCursorPos(Renderer *renderer, mpack_node_t cursor_goto) {
	mpack_node_t cursor_goto_params = mpack_node_array_at(cursor_goto, 1);
	renderer->cursor.row = MPackIntFromArray(cursor_goto_params, 1);
	renderer->cursor.col = MPackIntFromArray(cursor_goto_params, 2);
}

void UpdateCursorMode(Renderer *renderer, mpack_node_t mode_change) {
	mpack_node_t mode_change_params = mpack_node_array_at(mode_change, 1);
	renderer->cursor.mode_info = &renderer->cursor_mode_infos[mpack_node_array_at(mode_change_params, 1).data->value.u];
}

void UpdateCursorModeInfos(Renderer *renderer, mpack_node_t mode_info_set_params) {
	mpack_node_t mode_info_params = mpack_node_array_at(mode_info_set_params, 1);
	mpack_node_t mode_infos = mpack_node_array_at(mode_info_params, 1);
	size_t mode_infos_length = mpack_node_array_length(mode_infos);
	assert(mode_infos_length <= MAX_CURSOR_MODE_INFOS);

	for (size_t i = 0; i < mode_infos_length; ++i) {
		mpack_node_t mode_info_map = mpack_node_array_at(mode_infos, i);

		renderer->cursor_mode_infos[i].shape = CursorShape::None;
		mpack_node_t cursor_shape = mpack_node_map_cstr_optional(mode_info_map, "cursor_shape");
		if (!mpack_node_is_missing(cursor_shape)) {
			const char *cursor_shape_str = mpack_node_str(cursor_shape);
			size_t strlen = mpack_node_strlen(cursor_shape);
			if (!strncmp(cursor_shape_str, "block", strlen)) {
				renderer->cursor_mode_infos[i].shape = CursorShape::Block;
			}
			else if (!strncmp(cursor_shape_str, "vertical", strlen)) {
				renderer->cursor_mode_infos[i].shape = CursorShape::Vertical;
			}
			else if (!strncmp(cursor_shape_str, "horizontal", strlen)) {
				renderer->cursor_mode_infos[i].shape = CursorShape::Horizontal;
			}
		}

		renderer->cursor_mode_infos[i].cell_percentage = 0;
		mpack_node_t cell_percentage = mpack_node_map_cstr_optional(mode_info_map, "cell_percentage");
		if (!mpack_node_is_missing(cell_percentage)) {
			renderer->cursor_mode_infos[i].cell_percentage = static_cast<float>(cell_percentage.data->value.i) / 100.0f;
		}

		renderer->cursor_mode_infos[i].hl_attrib_index = 0;
		mpack_node_t hl_attrib_index = mpack_node_map_cstr_optional(mode_info_map, "attr_id");
		if (!mpack_node_is_missing(hl_attrib_index)) {
			renderer->cursor_mode_infos[i].hl_attrib_index = static_cast<int>(hl_attrib_index.data->value.i);
		}
	}
}

void ScrollRegion(Renderer *renderer, mpack_node_t scroll_region) {
	mpack_node_t scroll_region_params = mpack_node_array_at(scroll_region, 1);

	int64_t top = mpack_node_array_at(scroll_region_params, 1).data->value.i;
	int64_t bottom = mpack_node_array_at(scroll_region_params, 2).data->value.i;
	int64_t left = mpack_node_array_at(scroll_region_params, 3).data->value.i;
	int64_t right = mpack_node_array_at(scroll_region_params, 4).data->value.i;
	int64_t rows = mpack_node_array_at(scroll_region_params, 5).data->value.i;
	int64_t cols = mpack_node_array_at(scroll_region_params, 6).data->value.i;

	// Currently nvim does not support horizontal scrolling, 
	// the parameter is reserved for later use
	assert(cols == 0);

	// This part is slightly cryptic, basically we're just
	// iterating from top to bottom or vice versa depending on scroll direction.
	int64_t start_row = rows > 0 ? top : bottom - 1;
	int64_t end_row = rows > 0 ? bottom - 1 : top;
	int64_t increment = rows > 0 ? 1 : -1;

	for (int64_t i = start_row; rows > 0 ? i <= end_row : i >= end_row; i += increment) {
		// Clip anything outside the scroll region
		int64_t target_row = i - rows;
		if (target_row < top || target_row >= bottom) {
			continue;
		}

		memcpy(
			&renderer->grid_chars[target_row * renderer->grid_cols + left],
			&renderer->grid_chars[i * renderer->grid_cols + left],
			(right - left) * sizeof(wchar_t)
		);

		memcpy(
			&renderer->grid_cell_properties[target_row * renderer->grid_cols + left],
			&renderer->grid_cell_properties[i * renderer->grid_cols + left],
			(right - left) * sizeof(CellProperty)
		);
	}

	D2D1_RECT_U scroll_region_origin {
		.left = static_cast<uint32_t>(roundf(left * renderer->font_width)),
		.top = static_cast<uint32_t>(top * renderer->line_spacing),
		.right = static_cast<uint32_t>(roundf(right * renderer->font_width)),
		.bottom = static_cast<uint32_t>(bottom * renderer->line_spacing)
	};
	D2D1_RECT_F scroll_region_target {
		.left = roundf(left * renderer->font_width),
		.top = (top - rows) * renderer->line_spacing,
		.right = roundf(right * renderer->font_width),
		.bottom = (bottom - rows) * renderer->line_spacing
	};

	constexpr D2D1_POINT_2U dest_point { .x = 0, .y = 0 };
	renderer->scroll_region_bitmap->CopyFromRenderTarget(
		&dest_point,
		renderer->render_target,
		&scroll_region_origin
	);

	renderer->render_target->PushAxisAlignedClip(
		D2D1_RECT_F {
			.left = roundf(left * renderer->font_width),
			.top = top * renderer->line_spacing,
			.right = roundf(right * renderer->font_width),
			.bottom = bottom * renderer->line_spacing
		},
		D2D1_ANTIALIAS_MODE_ALIASED
	);

	D2D1_RECT_F bitmap_copy_rect {
		.left = 0.0f,
		.top = 0.0f,
		.right = scroll_region_target.right - scroll_region_target.left,
		.bottom = scroll_region_target.bottom - scroll_region_target.top
	};
	renderer->render_target->DrawBitmap(
		renderer->scroll_region_bitmap,
		&scroll_region_target,
		1.0f,
		D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
		&bitmap_copy_rect
	);
	renderer->render_target->PopAxisAlignedClip();
}

void DrawBorderRectangles(Renderer *renderer) {
	float left_border = renderer->font_width * renderer->grid_cols;
	float top_border = renderer->line_spacing * renderer->grid_rows;

	D2D1_RECT_F vertical_rect {
		.left = left_border,
		.top = 0.0f,
		.right = static_cast<float>(renderer->pixel_size.width),
		.bottom = static_cast<float>(renderer->pixel_size.height)
	};
	D2D1_RECT_F horizontal_rect {
		.left = 0.0f,
		.top = top_border,
		.right = static_cast<float>(renderer->pixel_size.width),
		.bottom = static_cast<float>(renderer->pixel_size.height)
	};

	DrawBackgroundRect(renderer, vertical_rect, &renderer->hl_attribs[0]);
	DrawBackgroundRect(renderer, horizontal_rect, &renderer->hl_attribs[0]);
}

void SetGuiOptions(Renderer *renderer, mpack_node_t option_set) {
	uint64_t option_set_length = mpack_node_array_length(option_set);

	for (uint64_t i = 1; i < option_set_length; ++i) {
		mpack_node_t name = mpack_node_array_at(mpack_node_array_at(option_set, i), 0);
		mpack_node_t value = mpack_node_array_at(mpack_node_array_at(option_set, i), 1);
		if (MPackMatchString(name, "guifont")) {
			size_t strlen = mpack_node_strlen(value);
			if (strlen == 0) {
				continue;
			}

			const char *font_str = mpack_node_str(value);
			const char *size_str = strstr(font_str, ":h");
			if (!size_str) {
				continue;
			}

			size_t font_str_len = size_str - font_str;
			size_t size_str_len = strlen - (font_str_len + 2);
			size_str += 2;
			
			assert(size_str_len < 64);
			char font_size[64];
			strncpy(font_size, size_str, size_str_len);
			font_size[size_str_len] = '\0';

			RendererUpdateFont(renderer, static_cast<float>(atof(font_size)), font_str, static_cast<int>(font_str_len));
			// Send message to window in order to update nvim row/col count
			PostMessage(renderer->hwnd, WM_RENDERER_FONT_UPDATE, 0, 0);
		}
	}
}

void ClearGrid(Renderer *renderer) {
	D2D1_RECT_F rect {
		.left = 0.0f,
		.top = 0.0f,
		.right = renderer->grid_cols * renderer->font_width,
		.bottom = renderer->grid_rows * renderer->line_spacing
	};
	DrawBackgroundRect(renderer, rect, &renderer->hl_attribs[0]);
}

void RendererRedraw(Renderer *renderer, mpack_node_t params) {
	if (!renderer->draw_active) {
		renderer->render_target->BeginDraw();
		renderer->render_target->SetTransform(D2D1::IdentityMatrix());
		renderer->draw_active = true;
	}

	//mpack_node_print_to_stdout(params);

	bool force_redraw = false;
	uint64_t redraw_commands_length = mpack_node_array_length(params);
	for (uint64_t i = 0; i < redraw_commands_length; ++i) {
		mpack_node_t redraw_command_arr = mpack_node_array_at(params, i);
		mpack_node_t redraw_command_name = mpack_node_array_at(redraw_command_arr, 0);

		if (MPackMatchString(redraw_command_name, "option_set")) {
			SetGuiOptions(renderer, redraw_command_arr);
		}
		if (MPackMatchString(redraw_command_name, "grid_resize")) {
			UpdateGridSize(renderer, redraw_command_arr);
		}
		if (MPackMatchString(redraw_command_name, "grid_clear")) {
			ClearGrid(renderer);
		}
		else if (MPackMatchString(redraw_command_name, "default_colors_set")) {
			UpdateDefaultColors(renderer, redraw_command_arr);
			ClearGrid(renderer);
		}
		else if (MPackMatchString(redraw_command_name, "hl_attr_define")) {
			UpdateHighlightAttributes(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "grid_line")) {
			DrawGridLines(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "grid_cursor_goto")) {
			EraseCursor(renderer);
			UpdateCursorPos(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "mode_info_set")) {
			UpdateCursorModeInfos(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "mode_change")) {
			UpdateCursorMode(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "grid_scroll")) {
			ScrollRegion(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "flush")) {
			DrawCursor(renderer);
			DrawBorderRectangles(renderer);
			renderer->render_target->EndDraw();
			renderer->draw_active = false;
		}
	}
}

CursorPos RendererTranslateMousePosToGrid(Renderer *renderer, POINTS mouse_pos) {
	return CursorPos {
		.row = static_cast<int>(mouse_pos.y / renderer->line_spacing),
		.col = static_cast<int>(mouse_pos.x / renderer->font_width)
	};
}