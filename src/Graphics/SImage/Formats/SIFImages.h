
#include "UI/WxUtils.h"
#include "Utility/Compression.h"
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <png.h>
#include <wx/mstream.h>
#include <wx/zipstrm.h>

class PNGChunk
{
public:
	PNGChunk(string_view name = "----") { memcpy(name_, name.data(), 4); }
	~PNGChunk() = default;

	string    name() const { return string(name_, 4); }
	uint32_t  size() const { return size_; }
	uint32_t  crc() const { return crc_; }
	MemChunk& data() { return data_; }

	void read(MemChunk& mc)
	{
		// Read size and chunk name
		mc.read(&size_, 4);
		mc.read(name_, 4);

		// Endianness correction
		size_ = wxUINT32_SWAP_ON_LE(size_);

		// Read chunk data
		data_.clear();
		mc.readMC(data_, size_);

		// Read crc
		mc.read(&crc_, 4);

		// Endianness correction
		crc_ = wxUINT32_SWAP_ON_LE(crc_);
	}

	void write(MemChunk& mc) const
	{
		// Endianness correction
		uint32_t size_swapped = wxUINT32_SWAP_ON_LE(size_);
		uint32_t crc_swapped  = wxUINT32_SWAP_ON_LE(crc_);

		// Write chunk data
		mc.write(&size_swapped, 4);
		mc.write(name_, 4);
		mc.write(data_.data(), size_);
		mc.write(&crc_swapped, 4);
	}

	void setData(const uint8_t* data, uint32_t size)
	{
		// Read given data
		data_.clear();
		data_.write(data, size);

		// Update variables
		size_ = size;

		// Note that the CRC is not computed just from
		// the chunk data, but also the chunk name! So
		// we need to write all that in a temporary MC
		MemChunk fulldata;
		fulldata.reSize(4 + size);
		fulldata.seek(0, SEEK_SET);
		fulldata.write(name_, 4);
		fulldata.write(data, size);
		crc_ = fulldata.crc();
		fulldata.clear();
	}

	void setData(MemChunk& mc) { setData(mc.data(), mc.size()); }

private:
	uint32_t size_ = 0;
	char     name_[4];
	MemChunk data_;
	uint32_t crc_ = 0;
};

class SIFAse : public SIFormat
{
public:
	SIFAse() : SIFormat("ase", "Aseprite", "ase", 255) {}

	bool isThisFormat(MemChunk& mc) override
	{
		if (mc.size() < 128)
			return false;

		const auto magic       = mc.readL16(4);
		const auto frame_count = mc.readL16(6);
		const auto width       = mc.readL16(8);
		const auto height      = mc.readL16(10);
		const auto depth       = mc.readL16(12);

		return magic == 0xA5E0 && frame_count > 0 && width > 0 && height > 0 && (depth == 8 || depth == 16 || depth == 32);
	}

	SImage::Info info(MemChunk& mc, int index) override
	{
		SImage::Info inf;
		inf.format     = "ase";
		inf.width      = mc.readL16(8);
		inf.height     = mc.readL16(10);
		inf.colformat  = SImage::Type::RGBA;
		inf.numimages  = mc.readL16(6);
		inf.imgindex   = std::clamp(index, 0, inf.numimages - 1);
		inf.offset_x   = inf.width / 2;
		inf.offset_y   = inf.height;
		inf.has_palette = false;
		return inf;
	}

protected:
	bool readImage(SImage& image, MemChunk& data, int index) override
	{
		ParsedAse parsed;
		if (!parseAse(data, parsed))
			return false;

		if (index < 0 || index >= parsed.num_frames)
			index = 0;

		auto frame_rgba = renderFrame(parsed, index);
		if (frame_rgba.empty())
		{
			global::error = "Unable to render Aseprite frame";
			return false;
		}

		image.create(parsed.width, parsed.height, SImage::Type::RGBA, nullptr, index, parsed.num_frames);
		image.setXOffset(parsed.width / 2);
		image.setYOffset(parsed.height);
		memcpy(imageData(image), frame_rgba.data(), frame_rgba.size());
		return true;
	}

private:
	struct AseLayer
	{
		uint16_t flags       = 0;
		uint16_t type        = 0;
		uint16_t child_level = 0;
		uint16_t blend_mode  = 0;
		uint8_t  opacity     = 255;
	};

	struct AseCel
	{
		bool            present       = false;
		bool            linked        = false;
		int             linked_frame  = -1;
		int             x             = 0;
		int             y             = 0;
		int             width         = 0;
		int             height        = 0;
		uint8_t         opacity       = 255;
		vector<uint8_t> rgba;
	};

	struct ParsedAse
	{
		int                      width             = 0;
		int                      height            = 0;
		int                      num_frames        = 0;
		int                      colour_depth      = 0;
		uint8_t                  transparent_index = 0;
		std::array<ColRGBA, 256> palette;
		bool                     has_palette = false;
		vector<AseLayer>         layers;
		vector<bool>             layer_visible;
		vector<vector<AseCel>>   frame_cels;
	};

	static void ensureCelStorage(ParsedAse& parsed, size_t layer_count)
	{
		for (auto& frame : parsed.frame_cels)
			if (frame.size() < layer_count)
				frame.resize(layer_count);
	}

	bool parseAse(MemChunk& mc, ParsedAse& parsed) const
	{
		if (mc.size() < 128 || mc.readL16(4) != 0xA5E0)
		{
			global::error = "Entry is not an Aseprite file";
			return false;
		}

		parsed.width             = mc.readL16(8);
		parsed.height            = mc.readL16(10);
		parsed.num_frames        = mc.readL16(6);
		parsed.colour_depth      = mc.readL16(12);
		parsed.transparent_index = mc[28];
		parsed.frame_cels.resize(parsed.num_frames);

		size_t offset = 128;
		for (int frame = 0; frame < parsed.num_frames; ++frame)
		{
			if (offset + 16 > mc.size())
			{
				global::error = "Truncated Aseprite frame header";
				return false;
			}

			auto frame_size = mc.readL32(offset);
			if (frame_size < 16 || offset + frame_size > mc.size())
			{
				global::error = "Invalid Aseprite frame size";
				return false;
			}

			if (mc.readL16(offset + 4) != 0xF1FA)
			{
				global::error = "Invalid Aseprite frame magic";
				return false;
			}

			auto chunk_count = mc.readL32(offset + 12);
			if (chunk_count == 0)
				chunk_count = mc.readL16(offset + 6);

			ensureCelStorage(parsed, parsed.layers.size());

			size_t pos       = offset + 16;
			size_t frame_end = offset + frame_size;
			for (uint32_t chunk = 0; chunk < chunk_count; ++chunk)
			{
				if (pos + 6 > frame_end)
				{
					global::error = "Truncated Aseprite chunk header";
					return false;
				}

				auto chunk_size = mc.readL32(pos);
				auto chunk_type = mc.readL16(pos + 4);
				if (chunk_size < 6 || pos + chunk_size > frame_end)
				{
					global::error = "Invalid Aseprite chunk size";
					return false;
				}

				auto chunk_pos = pos + 6;
				auto chunk_end = pos + chunk_size;
				switch (chunk_type)
				{
				case 0x2004:
				{
					if (chunk_end - chunk_pos < 16)
						break;

					AseLayer layer;
					layer.flags       = mc.readL16(chunk_pos);
					layer.type        = mc.readL16(chunk_pos + 2);
					layer.child_level = mc.readL16(chunk_pos + 4);
					layer.blend_mode  = mc.readL16(chunk_pos + 10);
					layer.opacity     = mc[chunk_pos + 12];
					parsed.layers.push_back(layer);
					ensureCelStorage(parsed, parsed.layers.size());
					break;
				}
				case 0x2005:
				{
					if (chunk_end - chunk_pos < 16)
						break;

					auto layer_index = mc.readL16(chunk_pos);
					if (parsed.layers.size() <= layer_index)
					{
						parsed.layers.resize(layer_index + 1);
						ensureCelStorage(parsed, parsed.layers.size());
					}

					auto& cel     = parsed.frame_cels[frame][layer_index];
					cel.present   = true;
					cel.linked    = false;
					cel.x         = static_cast<int16_t>(mc.readL16(chunk_pos + 2));
					cel.y         = static_cast<int16_t>(mc.readL16(chunk_pos + 4));
					cel.opacity   = mc[chunk_pos + 6];
					auto cel_type = mc.readL16(chunk_pos + 7);
					auto p        = chunk_pos + 16;

					if (cel_type == 1)
					{
						if (p + 2 > chunk_end)
							break;
						cel.linked       = true;
						cel.linked_frame = mc.readL16(p);
					}
					else if (cel_type == 0 || cel_type == 2)
					{
						if (p + 4 > chunk_end)
							break;

						cel.width  = mc.readL16(p);
						cel.height = mc.readL16(p + 2);
						p += 4;

						const size_t bpp          = parsed.colour_depth == 32 ? 4 : parsed.colour_depth == 16 ? 2 : 1;
						const size_t decoded_size = static_cast<size_t>(cel.width) * cel.height * bpp;
						if (cel.width <= 0 || cel.height <= 0 || decoded_size == 0)
							break;

						MemChunk pixel_data;
						if (cel_type == 0)
						{
							if (p + decoded_size > chunk_end)
								break;
							pixel_data.importMem(mc.data() + p, decoded_size);
						}
						else
						{
							MemChunk compressed;
							compressed.importMem(mc.data() + p, chunk_end - p);
							if (!compression::zlibInflate(compressed, pixel_data, decoded_size))
							{
								global::error = "Unable to decompress Aseprite cel data";
								return false;
							}
						}

						if (!decodeCel(pixel_data, parsed, cel))
							return false;
					}
					break;
				}
				case 0x2019:
				{
					if (chunk_end - chunk_pos < 20)
						break;

					auto first = mc.readL32(chunk_pos + 4);
					auto last  = mc.readL32(chunk_pos + 8);
					auto p     = chunk_pos + 20;
					for (uint32_t c = first; c <= last && c < parsed.palette.size() && p + 6 <= chunk_end; ++c)
					{
						auto flags = mc.readL16(p);
						p += 2;
						parsed.palette[c] = ColRGBA(mc[p], mc[p + 1], mc[p + 2], mc[p + 3]);
						parsed.has_palette = true;
						p += 4;
						if ((flags & 1) && p + 2 <= chunk_end)
						{
							auto name_len = mc.readL16(p);
							p += 2 + name_len;
							if (p > chunk_end)
								break;
						}
					}
					break;
				}
				default: break;
				}

				pos += chunk_size;
			}

			offset += frame_size;
		}

		computeLayerVisibility(parsed);
		return true;
	}

	static void computeLayerVisibility(ParsedAse& parsed)
	{
		parsed.layer_visible.clear();
		parsed.layer_visible.resize(parsed.layers.size(), true);

		vector<bool> group_visible_stack;
		for (size_t i = 0; i < parsed.layers.size(); ++i)
		{
			const auto& layer = parsed.layers[i];
			while (group_visible_stack.size() > layer.child_level)
				group_visible_stack.pop_back();

			const bool parent_visible = group_visible_stack.empty() ? true : group_visible_stack.back();
			const bool visible        = parent_visible && (layer.flags & 1) != 0;
			parsed.layer_visible[i]   = visible;
			if (layer.type == 1)
				group_visible_stack.push_back(visible);
		}
	}

	bool decodeCel(MemChunk& pixel_data, const ParsedAse& parsed, AseCel& cel) const
	{
		cel.rgba.clear();
		cel.rgba.resize(static_cast<size_t>(cel.width) * cel.height * 4, 0);

		if (parsed.colour_depth == 32)
		{
			if (pixel_data.size() < cel.rgba.size())
			{
				global::error = "Truncated Aseprite RGBA cel data";
				return false;
			}

			memcpy(cel.rgba.data(), pixel_data.data(), cel.rgba.size());
			return true;
		}
		else if (parsed.colour_depth == 16)
		{
			const size_t npixels = static_cast<size_t>(cel.width) * cel.height;
			if (pixel_data.size() < npixels * 2)
			{
				global::error = "Truncated Aseprite grayscale cel data";
				return false;
			}

			for (size_t i = 0; i < npixels; ++i)
			{
				auto value             = pixel_data[i * 2];
				cel.rgba[i * 4]     = value;
				cel.rgba[i * 4 + 1] = value;
				cel.rgba[i * 4 + 2] = value;
				cel.rgba[i * 4 + 3] = pixel_data[i * 2 + 1];
			}
			return true;
		}
		else if (parsed.colour_depth == 8)
		{
			const size_t npixels = static_cast<size_t>(cel.width) * cel.height;
			if (pixel_data.size() < npixels)
			{
				global::error = "Truncated Aseprite indexed cel data";
				return false;
			}
			if (!parsed.has_palette)
			{
				global::error = "Indexed Aseprite file has no palette";
				return false;
			}

			for (size_t i = 0; i < npixels; ++i)
			{
				auto index = pixel_data[i];
				auto col   = parsed.palette[index];
				if (index == parsed.transparent_index)
					col.a = 0;
				cel.rgba[i * 4]     = col.r;
				cel.rgba[i * 4 + 1] = col.g;
				cel.rgba[i * 4 + 2] = col.b;
				cel.rgba[i * 4 + 3] = col.a;
			}
			return true;
		}

		global::error = "Unsupported Aseprite colour depth";
		return false;
	}

	const AseCel* resolveCel(const ParsedAse& parsed, int frame, size_t layer) const
	{
		const AseCel* cel = &parsed.frame_cels[frame][layer];
		for (int guard = 0; guard < parsed.num_frames; ++guard)
		{
			if (!cel->linked)
				return cel->present ? cel : nullptr;
			if (cel->linked_frame < 0 || cel->linked_frame >= parsed.num_frames)
				return nullptr;
			cel = &parsed.frame_cels[cel->linked_frame][layer];
		}
		return nullptr;
	}

	static void blendPixel(uint8_t* dst, const uint8_t* src, uint8_t opacity)
	{
		const int sa = (src[3] * opacity + 127) / 255;
		if (sa <= 0)
			return;
		if (sa >= 255 && dst[3] == 0)
		{
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = 255;
			return;
		}

		const int da   = dst[3];
		const int outa = sa + ((da * (255 - sa) + 127) / 255);
		if (outa <= 0)
			return;

		const int src_r = src[0] * sa;
		const int src_g = src[1] * sa;
		const int src_b = src[2] * sa;
		const int dst_r = (dst[0] * da * (255 - sa) + 127) / 255;
		const int dst_g = (dst[1] * da * (255 - sa) + 127) / 255;
		const int dst_b = (dst[2] * da * (255 - sa) + 127) / 255;

		dst[0] = static_cast<uint8_t>((src_r + dst_r) / outa);
		dst[1] = static_cast<uint8_t>((src_g + dst_g) / outa);
		dst[2] = static_cast<uint8_t>((src_b + dst_b) / outa);
		dst[3] = static_cast<uint8_t>(outa);
	}

	vector<uint8_t> renderFrame(const ParsedAse& parsed, int frame) const
	{
		vector<uint8_t> rgba(static_cast<size_t>(parsed.width) * parsed.height * 4, 0);

		if (frame < 0 || frame >= parsed.num_frames)
			return rgba;

		for (size_t layer = 0; layer < parsed.layers.size(); ++layer)
		{
			if (layer >= parsed.frame_cels[frame].size() || !parsed.layer_visible[layer])
				continue;

			const auto& cel = parsed.frame_cels[frame][layer];
			if (!cel.present)
				continue;

			auto* source = resolveCel(parsed, frame, layer);
			if (!source || source->rgba.empty() || source->width <= 0 || source->height <= 0)
				continue;

			const auto& layer_info = parsed.layers[layer];
			const auto  opacity    = static_cast<uint8_t>((cel.opacity * layer_info.opacity + 127) / 255);

			for (int y = 0; y < source->height; ++y)
			{
				const int dy = cel.y + y;
				if (dy < 0 || dy >= parsed.height)
					continue;

				for (int x = 0; x < source->width; ++x)
				{
					const int dx = cel.x + x;
					if (dx < 0 || dx >= parsed.width)
						continue;

					auto*       dst = rgba.data() + (static_cast<size_t>(dy) * parsed.width + dx) * 4;
					const auto* src = source->rgba.data() + (static_cast<size_t>(y) * source->width + x) * 4;
					blendPixel(dst, src, opacity);
				}
			}
		}

		return rgba;
	}
};

namespace resprite
{
struct JsonValue
{
	enum class Type { Null, Boolean, Number, String, Object, Array } type = Type::Null;
	bool boolean = false;
	double number = 0;
	std::string string;
	std::map<std::string, JsonValue> object;
	std::vector<JsonValue> array;
};

class JsonParser
{
public:
	explicit JsonParser(const std::string& text) : text_{ text } {}

	bool parse(JsonValue& value)
	{
		return parseValue(value, 0) && (skipWhitespace(), position_ == text_.size());
	}

private:
	const std::string& text_;
	size_t position_ = 0;

	void skipWhitespace()
	{
		while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_])))
			++position_;
	}

	bool parseString(std::string& value)
	{
		if (position_ >= text_.size() || text_[position_++] != '"')
			return false;
		while (position_ < text_.size())
		{
			const char c = text_[position_++];
			if (c == '"')
				return true;
			if (static_cast<unsigned char>(c) < 0x20)
				return false;
			if (c != '\\')
			{
				value += c;
				continue;
			}
			if (position_ == text_.size())
				return false;
			switch (text_[position_++])
			{
			case '"': value += '"'; break;
			case '\\': value += '\\'; break;
			case '/': value += '/'; break;
			case 'b': value += '\b'; break;
			case 'f': value += '\f'; break;
			case 'n': value += '\n'; break;
			case 'r': value += '\r'; break;
			case 't': value += '\t'; break;
			case 'u':
			{
				if (position_ + 4 > text_.size()) return false;
				unsigned codepoint = 0;
				for (int i = 0; i < 4; ++i)
				{
					const char digit = text_[position_++];
					codepoint <<= 4;
					if (digit >= '0' && digit <= '9') codepoint |= digit - '0';
					else if (digit >= 'a' && digit <= 'f') codepoint |= digit - 'a' + 10;
					else if (digit >= 'A' && digit <= 'F') codepoint |= digit - 'A' + 10;
					else return false;
				}
				if (codepoint < 0x80) value += static_cast<char>(codepoint);
				else if (codepoint < 0x800)
				{
					value += static_cast<char>(0xC0 | (codepoint >> 6));
					value += static_cast<char>(0x80 | (codepoint & 0x3F));
				}
				else
				{
					value += static_cast<char>(0xE0 | (codepoint >> 12));
					value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
					value += static_cast<char>(0x80 | (codepoint & 0x3F));
				}
				break;
			}
			default: return false;
			}
		}
		return false;
	}

	bool parseValue(JsonValue& value, unsigned depth)
	{
		if (depth > 64) return false;
		skipWhitespace();
		if (position_ >= text_.size()) return false;
		const char c = text_[position_];
		if (c == '{')
		{
			value.type = JsonValue::Type::Object;
			++position_;
			skipWhitespace();
			if (position_ < text_.size() && text_[position_] == '}') { ++position_; return true; }
			while (position_ < text_.size())
			{
				std::string key;
				skipWhitespace();
				if (!parseString(key)) return false;
				skipWhitespace();
				if (position_ >= text_.size() || text_[position_++] != ':') return false;
				JsonValue child;
				if (!parseValue(child, depth + 1)) return false;
				value.object[std::move(key)] = std::move(child);
				skipWhitespace();
				if (position_ >= text_.size()) return false;
				if (text_[position_++] == '}') return true;
				if (text_[position_ - 1] != ',') return false;
			}
			return false;
		}
		if (c == '[')
		{
			value.type = JsonValue::Type::Array;
			++position_;
			skipWhitespace();
			if (position_ < text_.size() && text_[position_] == ']') { ++position_; return true; }
			while (position_ < text_.size())
			{
				JsonValue child;
				if (!parseValue(child, depth + 1)) return false;
				value.array.push_back(std::move(child));
				skipWhitespace();
				if (position_ >= text_.size()) return false;
				if (text_[position_++] == ']') return true;
				if (text_[position_ - 1] != ',') return false;
			}
			return false;
		}
		if (c == '"')
		{
			value.type = JsonValue::Type::String;
			return parseString(value.string);
		}
		if (text_.compare(position_, 4, "true") == 0)
		{
			position_ += 4; value.type = JsonValue::Type::Boolean; value.boolean = true; return true;
		}
		if (text_.compare(position_, 5, "false") == 0)
		{
			position_ += 5; value.type = JsonValue::Type::Boolean; value.boolean = false; return true;
		}
		if (text_.compare(position_, 4, "null") == 0) { position_ += 4; return true; }
		const char* start = text_.c_str() + position_;
		char* end = nullptr;
		value.number = std::strtod(start, &end);
		if (end == start || !std::isfinite(value.number)) return false;
		position_ += static_cast<size_t>(end - start);
		value.type = JsonValue::Type::Number;
		return true;
	}
};

const JsonValue* member(const JsonValue& value, const char* key)
{
	if (value.type != JsonValue::Type::Object) return nullptr;
	auto it = value.object.find(key);
	return it == value.object.end() ? nullptr : &it->second;
}

bool integer(const JsonValue& value, int& result)
{
	if (value.type != JsonValue::Type::Number || value.number < std::numeric_limits<int>::min()
		|| value.number > std::numeric_limits<int>::max() || std::floor(value.number) != value.number)
		return false;
	result = static_cast<int>(value.number);
	return true;
}

struct Document
{
	int width = 0;
	int height = 0;
	int frames = 0;
	std::string prefix;
	JsonValue json;
};

bool readArchive(MemChunk& data, Document& parsed, std::map<std::string, std::vector<uint8_t>>* cells = nullptr)
{
	if (data.size() < 4 || data.readL32(0) != 0x04034B50) return false;
	wxMemoryInputStream input(data.data(), data.size());
	wxZipInputStream zip(input);
	if (!zip.IsOk()) return false;
	std::string documentText;
	std::string documentPath;
	size_t cellBytes = 0;
	while (auto* entry = zip.GetNextEntry())
	{
		std::string path = entry->GetName(wxPATH_UNIX).utf8_string();
		std::replace(path.begin(), path.end(), '\\', '/');
		const auto slash = path.find_last_of('/');
		const bool isDocument = (slash == std::string::npos ? path : path.substr(slash + 1)) == "document.json";
		const bool isCell = cells && path.find("CellData/") != std::string::npos && path.size() > 4
			&& path.compare(path.size() - 4, 4, ".png") == 0;
		const auto size = entry->GetSize();
		if (isDocument && documentText.empty() && size > 0 && size <= 16 * 1024 * 1024)
		{
			std::vector<char> bytes(static_cast<size_t>(size));
			zip.Read(bytes.data(), bytes.size());
			if (zip.LastRead() == bytes.size())
			{
				documentText.assign(bytes.data(), bytes.size());
				documentPath = path;
			}
		}
		else if (isCell && size > 0 && size <= 64 * 1024 * 1024 && cellBytes + size <= 256 * 1024 * 1024)
		{
			std::vector<uint8_t> bytes(static_cast<size_t>(size));
			zip.Read(bytes.data(), bytes.size());
			if (zip.LastRead() == bytes.size())
			{
				cellBytes += bytes.size();
				(*cells)[path] = std::move(bytes);
			}
		}
		zip.CloseEntry();
		delete entry;
		if (!documentText.empty() && !cells) break;
	}
	if (documentText.empty()) return false;
	JsonParser parser{ documentText };
	if (!parser.parse(parsed.json) || parsed.json.type != JsonValue::Type::Object) return false;
	const auto* canvas = member(parsed.json, "canvasSize");
	const auto* width = canvas ? member(*canvas, "width") : nullptr;
	const auto* height = canvas ? member(*canvas, "height") : nullptr;
	const auto* frameCount = member(parsed.json, "frameCount");
	const auto* layers = member(parsed.json, "layerDatas");
	if (!width || !height || !frameCount || !layers || layers->type != JsonValue::Type::Array || layers->array.size() > 1024
		|| !integer(*width, parsed.width) || !integer(*height, parsed.height) || !integer(*frameCount, parsed.frames))
		return false;
	if (parsed.width <= 0 || parsed.height <= 0 || parsed.width > 8192 || parsed.height > 8192
		|| static_cast<uint64_t>(parsed.width) * parsed.height > 64ull * 1024 * 1024
		|| parsed.frames <= 0 || parsed.frames > 29
		|| static_cast<uint64_t>(parsed.width) * parsed.height * 4 * parsed.frames > 256ull * 1024 * 1024)
		return false;
	const auto slash = documentPath.find_last_of('/');
	if (slash != std::string::npos)
		parsed.prefix = documentPath.substr(0, slash + 1);
	return true;
}

uint8_t opacity(const JsonValue& object, const char* key)
{
	const auto* value = member(object, key);
	if (!value || value->type != JsonValue::Type::Number || !std::isfinite(value->number)) return 255;
	return static_cast<uint8_t>(std::lround(std::clamp(value->number, 0.0, 1.0) * 255.0));
}

bool uuid(const std::string& value)
{
	if (value.size() != 36) return false;
	for (size_t i = 0; i < value.size(); ++i)
	{
		if (i == 8 || i == 13 || i == 18 || i == 23)
		{
			if (value[i] != '-') return false;
		}
		else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
	}
	return true;
}
} // namespace resprite

class SIFResprite : public SIFormat
{
public:
	SIFResprite() : SIFormat("resprite", "Resprite", "resprite", 255) {}

	bool isThisFormat(MemChunk& data) override
	{
		resprite::Document document;
		return resprite::readArchive(data, document);
	}

	SImage::Info info(MemChunk& data, int index) override
	{
		SImage::Info result;
		result.format = "resprite";
		resprite::Document document;
		if (resprite::readArchive(data, document))
		{
			result.width = document.width;
			result.height = document.height;
			result.numimages = document.frames;
			result.imgindex = std::clamp(index, 0, document.frames - 1);
			result.offset_x = document.width / 2;
			result.offset_y = document.height;
		}
		return result;
	}

protected:
	bool readImage(SImage& image, MemChunk& data, int index) override
	{
		resprite::Document document;
		std::map<std::string, std::vector<uint8_t>> cellFiles;
		if (!resprite::readArchive(data, document, &cellFiles))
		{
			global::error = "Invalid Resprite project";
			return false;
		}
		if (index < 0 || index >= document.frames) index = 0;

		struct RasterCell { int width = 0; int height = 0; std::vector<uint8_t> rgba; };
		std::map<std::string, RasterCell> decodedCells;
		const auto* layers = resprite::member(document.json, "layerDatas");
		std::vector<uint8_t> canvas(static_cast<size_t>(document.width) * document.height * 4, 0);
		struct InheritedCell
		{
			std::string id;
			int baseX = 0, baseY = 0, x = 0, y = 0;
			uint8_t opacity = 255;
			bool hasSource = false;
		};
		std::vector<InheritedCell> inherited(layers->array.size());

		// Walk all preceding frames to resolve inherited cels, but composite only the requested frame.
		for (int currentFrame = 0; currentFrame <= index; ++currentFrame)
		{
		for (size_t layerIndex = layers->array.size(); layerIndex > 0; --layerIndex)
		{
			const size_t indexInLayers = layerIndex - 1;
			const auto& layer = layers->array[indexInLayers];
			if (layer.type != resprite::JsonValue::Type::Object) continue;
			const auto* group = resprite::member(layer, "isGroup");
			if (group && group->type == resprite::JsonValue::Type::Boolean && group->boolean) continue;
			const auto* kind = resprite::member(layer, "layerKind");
			if (kind && kind->type == resprite::JsonValue::Type::String && kind->string != "raster") continue;
			const auto* blend = resprite::member(layer, "blendMode");
			if (blend && blend->type == resprite::JsonValue::Type::String && blend->string != "bm-normal")
			{
				global::error = "Resprite blend modes other than normal are not supported";
				return false;
			}

			const auto* cells = resprite::member(layer, "cells");
			if (!cells || cells->type != resprite::JsonValue::Type::Array || static_cast<size_t>(currentFrame) >= cells->array.size())
			{
				inherited[indexInLayers].hasSource = false;
				continue;
			}
			const auto& cell = cells->array[currentFrame];
			if (cell.type != resprite::JsonValue::Type::Object)
			{
				inherited[indexInLayers].hasSource = false;
				continue;
			}
			auto& previous = inherited[indexInLayers];
			const auto* inherit = resprite::member(cell, "inherit");
			const bool isInherited = inherit && inherit->type == resprite::JsonValue::Type::Boolean && inherit->boolean;
			int x = 0, y = 0;
			if (isInherited)
			{
				if (!previous.hasSource) continue;
				x = previous.x;
				y = previous.y;
				const auto* offset = resprite::member(cell, "inheritOffset");
				const auto* offsetX = offset ? resprite::member(*offset, "x") : nullptr;
				const auto* offsetY = offset ? resprite::member(*offset, "y") : nullptr;
				int dx = 0, dy = 0;
				if (offsetX && offsetY && resprite::integer(*offsetX, dx) && resprite::integer(*offsetY, dy))
				{
					const int64_t nextX = int64_t(previous.baseX) + dx;
					const int64_t nextY = int64_t(previous.baseY) + dy;
					if (nextX < std::numeric_limits<int>::min() || nextX > std::numeric_limits<int>::max()
						|| nextY < std::numeric_limits<int>::min() || nextY > std::numeric_limits<int>::max())
						continue;
					x = static_cast<int>(nextX);
					y = static_cast<int>(nextY);
				}
				previous.x = x;
				previous.y = y;
			}
			else
			{
				const auto* id = resprite::member(cell, "id");
				const auto* bounds = resprite::member(cell, "bounds");
				const auto* origin = bounds ? resprite::member(*bounds, "origin") : nullptr;
				const auto* originX = origin ? resprite::member(*origin, "x") : nullptr;
				const auto* originY = origin ? resprite::member(*origin, "y") : nullptr;
				if (!id || id->type != resprite::JsonValue::Type::String || !resprite::uuid(id->string)
					|| !originX || !originY || !resprite::integer(*originX, x) || !resprite::integer(*originY, y))
				{
					previous.hasSource = false;
					continue;
				}
				previous.id = id->string;
				previous.baseX = previous.x = x;
				previous.baseY = previous.y = y;
				previous.hasSource = true;
			}
			const auto* cellOpacity = resprite::member(cell, "opacity");
			if (!isInherited || cellOpacity) previous.opacity = resprite::opacity(cell, "opacity");
			const auto* visible = resprite::member(layer, "contentVisible");
			if (visible && visible->type == resprite::JsonValue::Type::Boolean && !visible->boolean) continue;
			if (currentFrame != index) continue;

			const std::string cellPath = document.prefix + "CellData/" + previous.id + ".png";
			auto cached = decodedCells.find(cellPath);
			if (cached == decodedCells.end())
			{
				auto png = cellFiles.find(cellPath);
				if (png == cellFiles.end()) continue;
				MemChunk pngData;
				pngData.importMem(png->second.data(), static_cast<uint32_t>(png->second.size()));
				SImage pngImage;
				if (!pngImage.open(pngData) || pngImage.width() <= 0 || pngImage.height() <= 0 || pngImage.width() > 8192
					|| pngImage.height() > 8192 || static_cast<uint64_t>(pngImage.width()) * pngImage.height() > 64ull * 1024 * 1024)
					continue;
				MemChunk rgba;
				if (!pngImage.putRGBAData(rgba)) continue;
				RasterCell raster;
				raster.width = pngImage.width();
				raster.height = pngImage.height();
				raster.rgba.assign(rgba.data(), rgba.data() + rgba.size());
				cached = decodedCells.emplace(cellPath, std::move(raster)).first;
			}
			const auto& raster = cached->second;
			const unsigned layerOpacity = resprite::opacity(layer, "opacity");
			const unsigned opacity = (layerOpacity * previous.opacity + 127) / 255;
			const int64_t x0 = std::max<int64_t>(0, x), y0 = std::max<int64_t>(0, y);
			const int64_t x1 = std::min<int64_t>(document.width, int64_t(x) + raster.width);
			const int64_t y1 = std::min<int64_t>(document.height, int64_t(y) + raster.height);
			for (int64_t py = y0; py < y1; ++py)
			{
				for (int64_t px = x0; px < x1; ++px)
				{
					const auto* src = raster.rgba.data() + (static_cast<size_t>(py - y) * raster.width + static_cast<size_t>(px - x)) * 4;
					auto* dst = canvas.data() + (static_cast<size_t>(py) * document.width + static_cast<size_t>(px)) * 4;
					const unsigned sa = (unsigned(src[3]) * opacity + 127) / 255;
					if (sa == 0) continue;
					const unsigned da = dst[3];
					const unsigned outA = sa + (da * (255 - sa) + 127) / 255;
					for (int channel = 0; channel < 3; ++channel)
					{
						const unsigned source = unsigned(src[channel]) * sa;
						const unsigned dest = (unsigned(dst[channel]) * da * (255 - sa) + 127) / 255;
						dst[channel] = outA ? static_cast<uint8_t>((source + dest + outA / 2) / outA) : 0;
					}
					dst[3] = static_cast<uint8_t>(outA);
				}
			}
		}
		}
		image.create(document.width, document.height, SImage::Type::RGBA, nullptr, index, document.frames);
		image.setXOffset(document.width / 2);
		image.setYOffset(document.height);
		memcpy(imageData(image), canvas.data(), canvas.size());
		return true;
	}
};

// TODO: Keep PNG chunks in SImage so they are preserved between load/save
class SIFPng : public SIFormat
{
public:
	SIFPng() : SIFormat("png", "PNG", "png") {}

	bool isThisFormat(MemChunk& mc) override
	{
		// Reset MemChunk
		mc.seek(0, SEEK_SET);

		// Check size
		if (mc.size() > 8)
		{
			// Check for PNG header
			if (mc[0] == 137 && mc[1] == 80 && mc[2] == 78 && mc[3] == 71 && mc[4] == 13 && mc[5] == 10 && mc[6] == 26
				&& mc[7] == 10)
				return true;
		}

		return false;
	}

	SImage::Info info(MemChunk& mc, int index) override
	{
		SImage::Info inf;
		inf.format = "png";
		inf.width  = 0;
		inf.height = 0;

		// Read first chunk
		mc.seek(8, SEEK_SET);
		PNGChunk chunk;
		chunk.read(mc);
		// Should be IHDR
		int bpp = 32;
		if (chunk.name() == "IHDR")
		{
			// Read IHDR data
			IhdrChunk ihdr;
			chunk.data().read(&ihdr, 13, 0);

			// Set info from IHDR
			inf.width  = ihdr.width;
			inf.height = ihdr.height;
			bpp        = ihdr.bpp;
			if (ihdr.coltype == 3 && ihdr.bpp == 8)
			{
				// Only 8bpp 'indexed' pngs are counted as PalMask for now, all others will be converted to RGBA
				inf.colformat   = SImage::Type::PalMask;
				inf.has_palette = true;
			}
			else
				inf.colformat = SImage::Type::RGBA;
		}

		// Look for other info chunks (grAb or alPh)
		while (true)
		{
			chunk.read(mc);

			// Set format to alpha map if alPh present (and 8bpp)
			if (bpp == 8 && chunk.name() == "alPh")
				inf.colformat = SImage::Type::AlphaMap;

			// Set offsets if grAb present
			else if (chunk.name() == "grAb")
			{
				// Read offsets
				int32_t xoff, yoff;
				chunk.data().read(&xoff, 4, 0);
				chunk.data().read(&yoff, 4);
				inf.offset_x = wxINT32_SWAP_ON_LE(xoff);
				inf.offset_y = wxINT32_SWAP_ON_LE(yoff);
			}

			// Stop on IDAT chunk
			else if (chunk.name() == "IDAT")
				break;
		}

		return inf;
	}

	Writable canWrite(SImage& image) override
	{
		// PNG format is always writable
		return Writable::Yes;
	}

	bool canWriteType(SImage::Type type) override
	{
		// PNG format is always writable
		return true;
	}

	bool convertWritable(SImage& image, ConvertOptions opt) override
	{
		// Just convert to requested colour type

		// Paletted
		if (opt.col_format == SImage::Type::PalMask)
		{
			// Convert mask
			if (opt.mask_source == Mask::Alpha)
				image.cutoffMask(opt.alpha_threshold);
			else if (opt.mask_source == Mask::Colour)
				image.maskFromColour(opt.mask_colour, opt.pal_current);
			else
				image.fillAlpha(255);

			// Convert colours
			image.convertPaletted(opt.pal_target, opt.pal_current);
		}

		// RGBA
		else if (opt.col_format == SImage::Type::RGBA)
		{
			image.convertRGBA(opt.pal_current);

			// Convert alpha channel
			if (opt.mask_source == Mask::Colour)
				image.maskFromColour(opt.mask_colour, opt.pal_current);
			else if (opt.mask_source == Mask::Brightness)
				image.maskFromBrightness(opt.pal_current);
		}

		// Alpha Map
		else if (opt.col_format == SImage::Type::AlphaMap)
		{
			if (opt.mask_source == Mask::Alpha)
				image.convertAlphaMap(SImage::AlphaSource::Alpha, opt.pal_current);
			else if (opt.mask_source == Mask::Colour)
			{
				image.maskFromColour(opt.mask_colour, opt.pal_current);
				image.convertAlphaMap(SImage::AlphaSource::Alpha, opt.pal_current);
			}
			else
				image.convertAlphaMap(SImage::AlphaSource::Brightness, opt.pal_current);
		}

		// If transparency is disabled
		if (!opt.transparency)
			image.fillAlpha(255);

		return true;
	}

	bool writeOffset(SImage& image, ArchiveEntry* entry, Vec2i offset) override
	{
		MemChunk mc;
		image.setXOffset(offset.x);
		image.setYOffset(offset.y);
		return (writeImage(image, mc, nullptr, 0) && entry->importMemChunk(mc));
	}

protected:
	bool readImage(SImage& image, MemChunk& data, int index) override
	{
		// Check for grAb (offsets) and alPh chunks
		int32_t xoff = 0, yoff = 0;
		bool    alPh = false;
		{
			MemChunk scan;
			scan.importMem(data);
			scan.seek(8, SEEK_SET);
			PNGChunk chunk;
			while (true)
			{
				chunk.read(scan);
				if (chunk.name() == "grAb")
				{
					chunk.data().read(&xoff, 4, 0);
					chunk.data().read(&yoff, 4);
					xoff = wxINT32_SWAP_ON_LE(xoff);
					yoff = wxINT32_SWAP_ON_LE(yoff);
				}
				else if (chunk.name() == "alPh")
					alPh = true;
				if (chunk.name() == "IDAT")
					break;
			}
		}

		// Setup libpng for reading from memory
		png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
		if (!png_ptr)
		{
			global::error = "Error initialising libpng";
			return false;
		}
		png_infop info_ptr = png_create_info_struct(png_ptr);
		if (!info_ptr)
		{
			png_destroy_read_struct(&png_ptr, nullptr, nullptr);
			global::error = "Error initialising libpng info";
			return false;
		}
		if (setjmp(png_jmpbuf(png_ptr)))
		{
			png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
			global::error = "Error reading PNG data";
			return false;
		}

		PngReadContext ctx{ data.data(), data.size(), 0 };
		png_set_read_fn(png_ptr, &ctx, pngReadData);

		// Read header
		png_read_info(png_ptr, info_ptr);
		png_uint_32 width = 0, height = 0;
		int         bit_depth = 0, color_type = 0, interlace = 0, comp = 0, filter = 0;
		png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace, &comp, &filter);

		// Determine image type
		auto type = SImage::Type::RGBA;
		if (alPh)
			type = SImage::Type::AlphaMap;
		else if (color_type == PNG_COLOR_TYPE_PALETTE && bit_depth == 8)
			type = SImage::Type::PalMask;

		// Limit to 8-bit depth (SImage/Palette doesn't support higher, currently)
		if (bit_depth == 16)
			png_set_strip_16(png_ptr);

		// Setup png options based on type
		if (type == SImage::Type::PalMask)
		{
			if (bit_depth < 8)
				png_set_packing(png_ptr);
		}
		else if (type == SImage::Type::AlphaMap)
		{
			// Convert everything to 8-bit RGB so we can use red channel as alpha map
			if (color_type == PNG_COLOR_TYPE_PALETTE)
				png_set_palette_to_rgb(png_ptr);
			if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
				png_set_expand_gray_1_2_4_to_8(png_ptr);
			if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
				png_set_tRNS_to_alpha(png_ptr);
		}
		else
		{
			// RGBA
			if (color_type == PNG_COLOR_TYPE_PALETTE)
				png_set_palette_to_rgb(png_ptr);
			if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
				png_set_gray_to_rgb(png_ptr);
			if (bit_depth < 8)
				png_set_expand(png_ptr);
			if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
				png_set_tRNS_to_alpha(png_ptr);
			if (!(color_type & PNG_COLOR_MASK_ALPHA))
				png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
		}

		png_read_update_info(png_ptr, info_ptr);

		// Create image
		if (type == SImage::Type::PalMask)
		{
			Palette    pal(256);
			png_colorp plte = nullptr;
			int        num  = 0;
			if (png_get_PLTE(png_ptr, info_ptr, &plte, &num) & PNG_INFO_PLTE)
			{
				for (int i = 0; i < num && i < 256; ++i)
				{
					ColRGBA c{ plte[i].red, plte[i].green, plte[i].blue, 255 };
					pal.setColour(i, c);
				}
			}
			image.create(static_cast<int>(width), static_cast<int>(height), SImage::Type::PalMask, &pal);

			// Read indexed data
			vector<png_bytep> rows(height);
			for (png_uint_32 y = 0; y < height; ++y)
				rows[y] = imageData(image) + y * width;
			png_read_image(png_ptr, rows.data());

			// Build mask from tRNS if present
			png_bytep trans_alpha = nullptr;
			int       num_trans   = 0;
			if (png_get_tRNS(png_ptr, info_ptr, &trans_alpha, &num_trans, nullptr) & PNG_INFO_tRNS)
			{
				auto mask = imageMask(image);
				for (png_uint_32 y = 0; y < height; ++y)
				{
					for (png_uint_32 x = 0; x < width; ++x)
					{
						uint8_t idx         = rows[y][x];
						mask[y * width + x] = (idx < num_trans) ? trans_alpha[idx] : 255;
					}
				}
			}
			else
				image.fillAlpha(255);
		}
		else if (type == SImage::Type::AlphaMap)
		{
			image.create(static_cast<int>(width), static_cast<int>(height), SImage::Type::AlphaMap, nullptr);
			const auto      rowbytes = png_get_rowbytes(png_ptr, info_ptr);
			vector<uint8_t> row(rowbytes);
			for (png_uint_32 y = 0; y < height; ++y)
			{
				png_read_row(png_ptr, row.data(), nullptr);
				const int bpp = static_cast<int>(rowbytes / width);
				for (png_uint_32 x = 0; x < width; ++x)
					imageData(image)[y * width + x] = row[x * bpp];
			}
		}
		else // RGBA
		{
			image.create(static_cast<int>(width), static_cast<int>(height), SImage::Type::RGBA, nullptr);
			vector<png_bytep> rows(height);
			for (png_uint_32 y = 0; y < height; ++y)
				rows[y] = imageData(image) + y * width * 4;
			png_read_image(png_ptr, rows.data());
		}

		png_read_end(png_ptr, nullptr);
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

		// Set offsets
		image.setXOffset(xoff);
		image.setYOffset(yoff);

		return true;
	}

	bool writeImage(SImage& image, MemChunk& out, Palette* pal, int index) override
	{
		// Setup libpng for writing to memory
		png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
		if (!png_ptr)
			return false;
		png_infop info_ptr = png_create_info_struct(png_ptr);
		if (!info_ptr)
		{
			png_destroy_write_struct(&png_ptr, nullptr);
			return false;
		}
		if (setjmp(png_jmpbuf(png_ptr)))
		{
			png_destroy_write_struct(&png_ptr, &info_ptr);
			return false;
		}

		out.clear();
		PngWriteContext wctx{ &out };
		png_set_write_fn(png_ptr, &wctx, pngWriteData, pngFlush);

		auto type     = image.type();
		int  width    = image.width();
		int  height   = image.height();
		auto img_mask = imageMask(image);
		auto img_data = imageData(image);

		// IHDR
		int color_type = PNG_COLOR_TYPE_RGBA;
		int bit_depth  = 8;
		if (type == SImage::Type::PalMask)
			color_type = PNG_COLOR_TYPE_PALETTE;
		else if (type == SImage::Type::AlphaMap)
			color_type = PNG_COLOR_TYPE_GRAY;
		png_set_IHDR(
			png_ptr,
			info_ptr,
			width,
			height,
			bit_depth,
			color_type,
			PNG_INTERLACE_NONE,
			PNG_COMPRESSION_TYPE_DEFAULT,
			PNG_FILTER_TYPE_DEFAULT);

		// Palette
		if (type == SImage::Type::PalMask)
		{
			const auto used_pal = (pal && !image.hasPalette()) ? pal : image.palette();
			png_color  palette_plte[256]{};
			for (int i = 0; i < 256; ++i)
			{
				const auto c    = used_pal->colour(i);
				palette_plte[i] = { c.r, c.g, c.b };
			}
			png_set_PLTE(png_ptr, info_ptr, palette_plte, 256);

			// Build transparency info (if there is a mask)
			if (img_mask)
			{
				// Determine which palette index to use for transparent pixels
				// Start with the palette's existing transparent index if set
				int trans_index = used_pal->transIndex();
				
				// If no existing transparent index, find first unused colour in this image
				if (trans_index < 0)
				{
					trans_index = image.findUnusedColour(255);
					
					// Set any transparent pixels to this colour (if we found an unused colour)
					if (trans_index >= 0)
					{
						for (int a = 0; a < width * height; a++)
						{
							if (img_mask[a] == 0)
								img_data[a] = trans_index;
						}
					}
				}

				// Set tRNS chunk if we have a valid transparent index
				if (trans_index >= 0)
				{
					std::array<uint8_t, 256> trans;
					trans.fill(255);
					trans[trans_index] = 0;
					png_set_tRNS(png_ptr, info_ptr, trans.data(), trans.size(), nullptr);
				}
			}
		}

		png_write_info(png_ptr, info_ptr);

		// Write grAb/alPh chunks before IDAT
		if (image.offset().x != 0 || image.offset().y != 0)
		{
			uint8_t grab[8];
			int32_t x = wxINT32_SWAP_ON_LE((int32_t)image.offset().x);
			int32_t y = wxINT32_SWAP_ON_LE((int32_t)image.offset().y);
			memcpy(grab + 0, &x, 4);
			memcpy(grab + 4, &y, 4);
			png_write_chunk(png_ptr, reinterpret_cast<png_const_bytep>("grAb"), grab, 8);
		}
		if (type == SImage::Type::AlphaMap)
		{
			png_write_chunk(png_ptr, reinterpret_cast<png_const_bytep>("alPh"), nullptr, 0);
		}

		// Write rows
		if (type == SImage::Type::RGBA)
		{
			vector<png_bytep> rows(height);
			for (int y = 0; y < height; ++y)
				rows[y] = img_data + y * width * 4;
			png_write_image(png_ptr, rows.data());
		}
		else // PalMask or AlphaMap (both 1 byte per pixel)
		{
			vector<png_bytep> rows(height);
			for (int y = 0; y < height; ++y)
				rows[y] = img_data + y * width;
			png_write_image(png_ptr, rows.data());
		}

		png_write_end(png_ptr, info_ptr);
		png_destroy_write_struct(&png_ptr, &info_ptr);
		return true;
	}

private:
	// grAb chunk struct
	struct GrabChunk
	{
		int32_t xoff;
		int32_t yoff;
	};

	// IHDR chunk struct
	struct IhdrChunk
	{
		uint32_t width;
		uint32_t height;
		uint8_t  bpp;
		uint8_t  coltype;
		uint8_t  compression;
		uint8_t  filter;
		uint8_t  interlace;
	};

	// libpng IO helpers
	struct PngReadContext
	{
		const uint8_t* data;
		size_t         size;
		size_t         offset;
	};

	struct PngWriteContext
	{
		MemChunk* out;
	};

	static void pngReadData(png_structp png_ptr, png_bytep outBytes, png_size_t byteCount)
	{
		auto* ctx = static_cast<PngReadContext*>(png_get_io_ptr(png_ptr));
		if (!ctx || ctx->offset + byteCount > ctx->size)
			png_error(png_ptr, "png read beyond end of buffer");
		memcpy(outBytes, ctx->data + ctx->offset, byteCount);
		ctx->offset += byteCount;
	}

	static void pngWriteData(png_structp png_ptr, png_bytep inBytes, png_size_t byteCount)
	{
		auto* ctx = static_cast<PngWriteContext*>(png_get_io_ptr(png_ptr));
		if (!ctx || !ctx->out)
			png_error(png_ptr, "png write invalid context");
		ctx->out->write(inBytes, static_cast<uint32_t>(byteCount));
	}

	static void pngFlush(png_structp)
	{
		// Not needed for MemChunk
	}
};
