#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Minimal XML reader for the game's MyGUI files (skins, image sets, fonts, colours, layouts): elements with attributes and children, a UTF-8 BOM,
// comments, <?...?> and <!...> skipped; text content is ignored (MyGUI keeps everything in attributes). Entities: &amp; &lt; &gt; &quot; &apos;.
namespace ui::xml
{
	struct Node
	{
		std::string name;
		std::vector<std::pair<std::string, std::string>> attrs;
		std::vector<std::unique_ptr<Node>> children;

		const std::string& attr(const char* key) const;	// "" when absent
		bool has(const char* key) const;
		std::vector<const Node*> all(const char* childName) const;	// direct children of that name
		const Node* first(const char* childName) const;
	};

	// The document's root element, or null with `err` set.
	std::unique_ptr<Node> parse(const std::string& text, std::string& err);
	std::unique_ptr<Node> load(const std::string& path, std::string& err);
}
