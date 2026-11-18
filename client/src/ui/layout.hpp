#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// Layout files (docs/UI.md "Layout files"; the format: client/data/ui/README.md): a screen that opts in describes its widget tree in
// <dir>/ui/<name>.layout, our own indented format over Flex / Tabs / ScrollView / Panel / Label / Button; the screen looks widgets up by
// id and binds them in onBuild. With ui.layout_reload a saved file rebuilds the tree live (onBuild runs again; a slot's children move
// over). A missing or bad file fails loudly: file:line on stderr and a red box in the layout's place; a bad reload keeps the last good
// tree under the box. Nothing here reads the screen size.
namespace ui
{
	// The host's data dir (client/data, found from the executable); files are read from <dataDir>/ui/. Call before building a screen
	// that uses a layout file.
	void setLayoutDir(const std::string& dataDir);

	class Layout
	{
	public:
		// Builds <dataDir>/ui/<name>.layout into parent now; onBuild runs after each good build (bind ids, fill slots).
		Layout(Widget& parent, std::string name, std::function<void(Layout&)> onBuild = {});
		~Layout();
		Layout(const Layout&) = delete;
		Layout& operator=(const Layout&) = delete;

		Widget* find(const std::string& id) const;
		// The widget by id as a T. Missing or another kind: an error (shown in the red box) and a spare T outside the tree, so the screen
		// keeps working.
		template<class T> T& at(const std::string& id)
		{
			if (T* w = dynamic_cast<T*>(find(id))) { return *w; }
			missing(id);
			auto s = std::make_unique<T>();
			T& r = *s;
			spares_.push_back(std::move(s));
			return r;
		}
		bool ok() const { return error_.empty(); }
		const std::string& error() const { return error_; }	// "file:line: what"; "" when fine
		const std::string& path() const { return path_; }
		void reload();	// now (tests); else a changed file reloads by itself
		static void pollAll();	// ui::frame: reloads the changed files (ui.layout_reload)

	private:
		struct Node;
		bool build();
		void missing(const std::string& id);
		void showError();
		Widget& parent_;
		std::string name_, path_, error_;
		std::function<void(Layout&)> onBuild_;
		std::vector<Widget*> roots_;	// the file's top-level widgets in parent_
		std::map<std::string, Widget*> ids_;
		std::set<std::string> slots_;	// ids of slot nodes (their children are the screen's)
		std::vector<std::unique_ptr<Widget>> spares_;
		Widget* errorBox_ = nullptr;
		std::filesystem::file_time_type stamp_{};
	};
}
