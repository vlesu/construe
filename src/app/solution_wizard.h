// Connection wizard (spec/app-ux.md §8д, solutions.md §8; S6-SOL-002…008): the solution list, the inputs picked in
// the view, the choices limited to suitable variants and the plan of the connection (chosen rows, holes, fasteners).
// Commands themselves belong to the main window.
#pragma once

#include "api/session.h"
#include "app/slot_tree.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

// The plan in words (§8д «Summary»): cuts «<title> Ø<diameter> [thread] [fit]» and parts «<title>: <designation>».
QStringList solutionPlanLines(const api::Json& report, const api::Session* s = nullptr);

class SolutionWizard : public QWidget {
    Q_OBJECT
public:
    explicit SolutionWizard(api::Session& session, QWidget* parent = nullptr);
    // A new connection (editing empty) or editing a connection feature.
    void start(const std::optional<std::string>& editing);
    const std::optional<std::string>& editing() const { return editing_; }
    std::string packageId() const;  // the chosen solution of the catalog (a new connection)
    // Inputs for the command: a new connection with clicked points (FX8-BLT-002) gives the point input as the list of all
    // points [[x, y, z], …] — the command makes a sketch of them on the head face.
    api::Json inputs() const;
    // Points clicked on the head face of a new connection (§8д, FX8-BLT-002): a bolt at each.
    const std::vector<std::array<double, 3>>& points() const { return points_; }
    // Axes of the connection (§8д, FX7-ASM-003, S7F-ASM-008, FX10-WZ-005): every hole axis picked for the solution's frame
    // axis input; more than one — one connection with a bolt on each (applySolutionAxes; editing — updateSolutionAxes).
    // The first is also inputs()[frame axis].
    const std::vector<std::string>& axes() const { return axes_; }
    api::Json choices() const { return choices_; }
    // Where to cut (S6-SOL-010, solutions.md §7.10): "assembly" or "parts"; a new connection starts with defaultCut.
    std::string cut() const;
    void setDefaultCut(const std::string& c) { defaultCut_ = c; }
    bool canAccept() const;
    // Variants still checked in the background (solutions.md §8 item 3, FX8-BLT-001); the window is not idle meanwhile.
    bool checkPending() const { return !pendingChecks_.empty(); }
    // A face picked in the view: assembly reference "<path>/<name>", its surface kind and the click point (if known).
    // Assigned to the current place (a point input takes the face for its face input too); then the next unset place.
    void picked(const std::string& ref, std::optional<kernel::SurfaceInfo::Kind> kind, const std::optional<std::array<double, 3>>& at);
    // The current place takes an axis (a cylinder input, S7F-ASM-008).
    bool axisPlaceCurrent() const;
    // Hole faces of an axis given otherwise (a circular edge, a reference axis, a sketch point — Session::holeAxisFaces)
    // for the current axis place: the frame axis — each one not chosen is added, a single chosen one picked again is
    // removed (toggle); another cylinder input — the first one is set.
    void pickedAxes(const std::vector<std::string>& faces, bool toggle = true);
    // The head face chosen (the frame face, for preferring its part among coaxial holes); empty — none.
    std::string frameFaceRef() const;
    // The status line hint (S7F-UI-006): what to pick for the current place, with its title.
    QString hint() const;
    SlotTree* places() const { return slots_; }
    void setMessage(const QString& m);
    // A language switch (library.md §17.2): solution titles, places, choices and the plan in the new language; the picked
    // inputs and the choices stay.
    void retranslate();
    // Assembly references of the chosen faces (for highlighting in the view).
    std::vector<std::string> chosenFaces() const;
    // Instance caption for a reference (set by the main window, like the mate panel).
    void setRefLabeler(std::function<QString(const std::string&)> f) { labeler_ = std::move(f); }

signals:
    void sameHolesRequested();  // «All identical holes of the part» (S7F-ASM-008): the main window resolves the pattern
    void accepted();
    void cancelled();
    void changed();  // inputs, choices or the plan changed (availability of ✓, highlighting)

private:
    void fillSolutions();
    void solutionChanged();
    void buildForm();
    void replan();
    void checkNext();  // one unchecked variant (FX8-BLT-001)
    // The plan waits for the pack end face (FX8-BLT-003): a new connection of a solution with frame.packEnd not set.
    bool waitsForPackEnd() const;
    void fillInputs();  // the places tree from the inputs, the axes and the points
    void removed(int slot, const std::string& ref);  // ✕ at an object of the places tree
    void toggleAxis(const std::string& ref, bool toggle);
    int currentPlace() const;
    const solutions::Definition* definition() const;
    QString optionLabel(const solutions::ChoiceDef& c, const std::string& value, const std::string& label) const;
    std::vector<SlotSpec> placeSpecs() const;  // the places of the inputs (buildForm)

    api::Session& s_;
    std::optional<std::string> editing_;
    std::shared_ptr<const solutions::Catalog> catalog_;
    std::optional<solutions::Definition> def_;
    api::Json inputs_ = api::Json::object(), choices_ = api::Json::object();
    std::optional<api::Session::SolutionPlan> plan_;
    std::vector<std::string> axes_;
    std::vector<std::array<double, 3>> points_;
    bool axesOk_ = true;  // the plans of the second and further axes have no error
    bool syncing_ = false;
    std::vector<std::pair<std::string, std::string>> pendingChecks_;  // (choice, value) not checked yet
    QTimer* checkTimer_ = nullptr;
    std::function<QString(const std::string&)> labeler_;
    QLabel* title_ = nullptr;
    QComboBox* solution_ = nullptr;
    SlotTree* slots_ = nullptr;
    QPushButton* sameHoles_ = nullptr;
    QWidget* choicesBox_ = nullptr;
    QFormLayout* choicesForm_ = nullptr;
    std::vector<QWidget*> choiceWidgets_;
    QComboBox* cut_ = nullptr;
    std::string defaultCut_ = "assembly";
    QLabel* result_ = nullptr;
    QLabel* message_ = nullptr;
    QPushButton* ok_ = nullptr;
};

}  // namespace construe::app
