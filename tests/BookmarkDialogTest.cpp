#include <ImGuiFileDialog.h>

#include <cassert>
#include <string>

int main() {
    IGFD::FileDialog dialog;
    assert(dialog.AddPlacesGroup("Bookmarks", 0, true, true));

    const std::string saved =
        "###Bookmarks###Cinema##/media/Cinema##Series##/media/Series";
    dialog.DeserializePlaces(saved);
    assert(dialog.SerializePlaces() == saved);

    // Regression: OpenDialog() initializes Places every time. It must retain
    // an already-deserialized group rather than replacing it with an empty
    // one (the bug that erased bookmarks on the next explorer opening).
    IGFD::FileDialogConfig config;
    config.path = ".";
    dialog.OpenDialog("open_media", "Open media", "All files{.*}", config);
    assert(dialog.SerializePlaces() == saved);
    dialog.Close();

    dialog.OpenDialog("open_media", "Open media", "All files{.*}", config);
    assert(dialog.SerializePlaces() == saved);
    return 0;
}
