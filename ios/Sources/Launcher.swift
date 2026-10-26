import GameController
import UIKit
import UniformTypeIdentifiers

// MARK: - Settings the launcher passes to the game

/// Each setting is a cvar the game or the SDK already has; the launcher stores the choice and
/// hands it to the game host at Play (mcla_game_set_option), after the saved settings file.
struct LaunchChoice {
    let key: String
    let title: String
    let detail: String
    let cvar: String
    let options: [(label: String, value: String)]
    let defaultValue: String

    var value: String {
        get { UserDefaults.standard.string(forKey: "launch." + key) ?? defaultValue }
        nonmutating set { UserDefaults.standard.set(newValue, forKey: "launch." + key) }
    }
}

struct LaunchToggle {
    let key: String
    let title: String
    let detail: String
    let cvar: String
    let defaultValue: Bool

    var isOn: Bool {
        get { UserDefaults.standard.object(forKey: "launch." + key) as? Bool ?? defaultValue }
        nonmutating set { UserDefaults.standard.set(newValue, forKey: "launch." + key) }
    }
}

enum LaunchSettings {
    static let frameRate = LaunchChoice(
        key: "fps", title: "Frame rate",
        detail: "30 is the game's own rate. 60 and 120 need headroom the A12X may not have in the city.",
        cvar: "mcla_fps", options: [("30", "30"), ("60", "60"), ("120", "120")], defaultValue: "30")
    static let resolution = LaunchChoice(
        key: "scale", title: "Resolution",
        detail: "The game draws at 1280x720; 2x draws at 2560x1440 and costs about four times the GPU work.",
        cvar: "resolution_scale", options: [("720p", "1"), ("1440p (2x)", "2")], defaultValue: "1")
    static let intro = LaunchChoice(
        key: "intro", title: "Start-up logos",
        detail: "Skip also removes the logo from the title screen.",
        cvar: "mcla_intro", options: [("Normal", "normal"), ("Fast", "fast"), ("Skip", "skip")],
        defaultValue: "normal")

    static let choices = [frameRate, resolution, intro]

    static let toggles = [
        LaunchToggle(key: "vsync", title: "V-sync", detail: "Off can tear but reacts sooner",
                     cvar: "vsync", defaultValue: true),
        LaunchToggle(key: "shadows", title: "Shadows", detail: "Off is the largest saving",
                     cvar: "mcla_shadows", defaultValue: true),
        LaunchToggle(key: "motionBlur", title: "Motion blur", detail: "",
                     cvar: "mcla_motion_blur", defaultValue: true),
        LaunchToggle(key: "dof", title: "Depth of field", detail: "",
                     cvar: "mcla_depth_of_field", defaultValue: true),
        LaunchToggle(key: "msaa", title: "Anti-aliasing (MSAA)", detail: "",
                     cvar: "mcla_msaa", defaultValue: true),
        LaunchToggle(key: "blur", title: "Full-screen blur", detail: "",
                     cvar: "mcla_fullscreen_blur", defaultValue: true),
    ]

    static func applyToGame() {
        for choice in choices { mcla_game_set_option(choice.cvar, choice.value) }
        for toggle in toggles { mcla_game_set_option(toggle.cvar, toggle.isOn ? "true" : "false") }
    }

    static var summary: String {
        let fps = frameRate.value
        let res = resolution.options.first { $0.value == resolution.value }?.label ?? "720p"
        return "\(fps) fps · \(res)"
    }
}

// MARK: - Look

enum Theme {
    static let accent = UIColor(red: 1.0, green: 0.62, blue: 0.24, alpha: 1)
    static let card = UIColor(white: 0.16, alpha: 0.82)
    static let cardBorder = UIColor(white: 1, alpha: 0.10)
    static let secondaryText = UIColor(white: 1, alpha: 0.68)

    /// Neon sunset behind the launcher (the game's own art stays on the owner's disc).
    static func backgroundLayer() -> CAGradientLayer {
        let layer = CAGradientLayer()
        layer.colors = [
            UIColor(red: 0.05, green: 0.04, blue: 0.16, alpha: 1).cgColor,
            UIColor(red: 0.20, green: 0.06, blue: 0.32, alpha: 1).cgColor,
            UIColor(red: 0.62, green: 0.12, blue: 0.42, alpha: 1).cgColor,
            UIColor(red: 0.98, green: 0.45, blue: 0.20, alpha: 1).cgColor,
        ]
        layer.locations = [0, 0.45, 0.75, 1]
        layer.startPoint = CGPoint(x: 0.2, y: 0)
        layer.endPoint = CGPoint(x: 0.8, y: 1)
        return layer
    }

    static func makeCard() -> UIView {
        let card = UIView()
        card.backgroundColor = Theme.card
        card.layer.cornerRadius = 18
        card.layer.borderWidth = 1
        card.layer.borderColor = cardBorder.cgColor
        return card
    }
}

// MARK: - Home screen

final class LauncherViewController: UIViewController {
    private let background = Theme.backgroundLayer()
    private let artView = UIImageView()
    private let shadeView = UIView()
    private let shade = CAGradientLayer()
    private let filesIcon = UIImageView()
    private let filesTitle = UILabel()
    private let filesDetail = UILabel()
    private let playButton = UIButton(type: .system)
    private let playNote = UILabel()
    private let controllerLabel = UILabel()
    private var gameFolder: URL?

    override var preferredStatusBarStyle: UIStatusBarStyle { .lightContent }
    override var prefersHomeIndicatorAutoHidden: Bool { true }

    override func viewDidLoad() {
        super.viewDidLoad()
        overrideUserInterfaceStyle = .dark
        view.backgroundColor = .black
        view.layer.insertSublayer(background, at: 0)
        // The game's dashboard art from the owner's own files, when it can be read.
        artView.contentMode = .scaleAspectFill
        artView.clipsToBounds = true
        artView.alpha = 0
        view.addSubview(artView)
        // Darker on the left, where the text is.
        shade.colors = [UIColor(white: 0, alpha: 0.78).cgColor, UIColor(white: 0, alpha: 0.35).cgColor,
                        UIColor(white: 0, alpha: 0).cgColor]
        shade.locations = [0, 0.45, 0.8]
        shade.startPoint = CGPoint(x: 0, y: 0.5)
        shade.endPoint = CGPoint(x: 1, y: 0.5)
        shadeView.layer.addSublayer(shade)
        shadeView.isUserInteractionEnabled = false
        view.addSubview(shadeView)

        buildTopButtons()
        buildColumn()

        NotificationCenter.default.addObserver(self, selector: #selector(updateController),
                                               name: .GCControllerDidConnect, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(updateController),
                                               name: .GCControllerDidDisconnect, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(refresh),
                                               name: UIApplication.didBecomeActiveNotification, object: nil)
        log("launcher: \(mcla_game_available() != 0 ? "game build" : "public build (no game code)")")
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        navigationController?.setNavigationBarHidden(true, animated: animated)
        refresh()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        background.frame = view.bounds
        artView.frame = view.bounds
        shadeView.frame = view.bounds
        shade.frame = view.bounds
    }

    private func buildTopButtons() {
        let stack = UIStackView()
        stack.axis = .horizontal
        stack.spacing = 14
        stack.translatesAutoresizingMaskIntoConstraints = false
        for (symbol, action, label) in [("doc.text", #selector(openLog), "Log"),
                                        ("gearshape.fill", #selector(openSettings), "Settings")] {
            let button = UIButton(type: .system)
            button.setImage(UIImage(systemName: symbol,
                                    withConfiguration: UIImage.SymbolConfiguration(pointSize: 20, weight: .semibold)),
                            for: .normal)
            button.tintColor = .white
            button.backgroundColor = UIColor(white: 1, alpha: 0.14)
            button.layer.cornerRadius = 26
            button.accessibilityLabel = label
            button.addTarget(self, action: action, for: .touchUpInside)
            button.widthAnchor.constraint(equalToConstant: 52).isActive = true
            button.heightAnchor.constraint(equalToConstant: 52).isActive = true
            stack.addArrangedSubview(button)
        }
        view.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -20),
        ])
    }

    private func buildColumn() {
        let column = UIStackView()
        column.axis = .vertical
        column.spacing = 16
        column.translatesAutoresizingMaskIntoConstraints = false

        // Title row: icon, name, subtitle.
        let icon = UIImageView(image: Self.launcherIcon())
        icon.contentMode = .scaleAspectFill
        icon.clipsToBounds = true
        icon.layer.cornerRadius = 18
        icon.tintColor = Theme.accent
        icon.backgroundColor = UIColor(white: 1, alpha: 0.08)
        icon.widthAnchor.constraint(equalToConstant: 84).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 84).isActive = true
        let name = UILabel()
        name.text = "MCLA Port"
        name.font = .systemFont(ofSize: 40, weight: .heavy)
        name.textColor = .white
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        let subtitle = UILabel()
        subtitle.text = "Fan project · plays your own game files · v\(version)"
        subtitle.font = .systemFont(ofSize: 17, weight: .medium)
        subtitle.textColor = Theme.secondaryText
        subtitle.numberOfLines = 0
        let titles = UIStackView(arrangedSubviews: [name, subtitle])
        titles.axis = .vertical
        titles.spacing = 2
        let header = UIStackView(arrangedSubviews: [icon, titles])
        header.spacing = 18
        header.alignment = .center
        column.addArrangedSubview(header)
        column.setCustomSpacing(26, after: header)

        // Game files card.
        let card = Theme.makeCard()
        filesIcon.contentMode = .scaleAspectFit
        filesIcon.widthAnchor.constraint(equalToConstant: 30).isActive = true
        filesTitle.font = .systemFont(ofSize: 19, weight: .semibold)
        filesTitle.textColor = .white
        filesDetail.font = .systemFont(ofSize: 15)
        filesDetail.textColor = Theme.secondaryText
        filesDetail.numberOfLines = 0
        let chevron = UIImageView(image: UIImage(systemName: "chevron.right"))
        chevron.tintColor = Theme.secondaryText
        chevron.setContentHuggingPriority(.required, for: .horizontal)
        let texts = UIStackView(arrangedSubviews: [filesTitle, filesDetail])
        texts.axis = .vertical
        texts.spacing = 3
        let row = UIStackView(arrangedSubviews: [filesIcon, texts, chevron])
        row.spacing = 16
        row.alignment = .center
        row.isUserInteractionEnabled = false
        row.translatesAutoresizingMaskIntoConstraints = false
        card.addSubview(row)
        NSLayoutConstraint.activate([
            row.topAnchor.constraint(equalTo: card.topAnchor, constant: 16),
            row.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -16),
            row.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 20),
            row.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -18),
        ])
        card.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(openGameFiles)))
        column.addArrangedSubview(card)

        // Play.
        var config = UIButton.Configuration.filled()
        config.baseBackgroundColor = Theme.accent
        config.baseForegroundColor = UIColor(white: 0.08, alpha: 1)
        config.cornerStyle = .capsule
        config.image = UIImage(systemName: "play.fill",
                               withConfiguration: UIImage.SymbolConfiguration(pointSize: 24, weight: .bold))
        config.imagePadding = 14
        var title = AttributedString("Play")
        title.font = .systemFont(ofSize: 28, weight: .bold)
        config.attributedTitle = title
        playButton.configuration = config
        playButton.heightAnchor.constraint(equalToConstant: 78).isActive = true
        playButton.layer.shadowColor = Theme.accent.cgColor
        playButton.layer.shadowOpacity = 0.45
        playButton.layer.shadowRadius = 18
        playButton.addTarget(self, action: #selector(play), for: .touchUpInside)
        column.addArrangedSubview(playButton)

        playNote.font = .systemFont(ofSize: 15)
        playNote.textColor = Theme.secondaryText
        playNote.numberOfLines = 0
        controllerLabel.font = .systemFont(ofSize: 15, weight: .medium)
        controllerLabel.textColor = Theme.secondaryText
        controllerLabel.numberOfLines = 0
        column.addArrangedSubview(playNote)
        column.setCustomSpacing(6, after: playNote)
        column.addArrangedSubview(controllerLabel)

        view.addSubview(column)
        let wide = column.widthAnchor.constraint(equalTo: view.widthAnchor, multiplier: 0.42)
        wide.priority = .defaultHigh
        NSLayoutConstraint.activate([
            column.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 44),
            column.centerYAnchor.constraint(equalTo: view.centerYAnchor, constant: 20),
            wide,
            column.widthAnchor.constraint(greaterThanOrEqualToConstant: 340),
            column.trailingAnchor.constraint(lessThanOrEqualTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -24),
        ])
    }

    /// The app icon, bundled as LauncherIcon.png by the private build; a symbol otherwise.
    static func launcherIcon() -> UIImage? {
        if let url = Bundle.main.url(forResource: "LauncherIcon", withExtension: "png"),
           let image = UIImage(contentsOfFile: url.path) {
            return image
        }
        return UIImage(systemName: "car.side.fill")
    }

    private func loadArt() {
        guard artView.image == nil else { return }
        GameArt.loadBackground { [weak self] image in
            guard let self, let image else { return }
            self.artView.image = image
            UIView.animate(withDuration: 0.6) { self.artView.alpha = 1 }
        }
    }

    @objc func refresh() {
        gameFolder = GameFolder.locate()
        if gameFolder != nil { loadArt() }
        let hasGame = mcla_game_available() != 0
        if let folder = gameFolder {
            filesIcon.image = UIImage(systemName: "checkmark.circle.fill")
            filesIcon.tintColor = .systemGreen
            filesTitle.text = "Midnight Club: Los Angeles"
            let place = folder.lastPathComponent.lowercased() == "game"
                ? "On My iPad › MCLA › \(folder.lastPathComponent)" : "the imported folder"
            filesDetail.text = "default.xex found in \(place)"
        } else {
            filesIcon.image = UIImage(systemName: "exclamationmark.circle.fill")
            filesIcon.tintColor = .systemOrange
            filesTitle.text = "Game files not found"
            filesDetail.text = "Copy your game folder (with default.xex) to On My iPad › MCLA › game"
        }
        let canPlay = hasGame && gameFolder != nil
        playButton.isEnabled = canPlay
        playButton.alpha = canPlay ? 1 : 0.45
        if !hasGame {
            playNote.text = "This build has no game code (public build). The private build adds it."
        } else {
            playNote.text = "\(LaunchSettings.summary). Change in Settings › Graphics."
        }
        updateController()
    }

    @objc private func updateController() {
        let pads = GCController.controllers().filter { $0.extendedGamepad != nil }
        if let pad = pads.first {
            controllerLabel.text = "Controller: \(pad.vendorName ?? "connected")"
        } else {
            controllerLabel.text = "No controller. Pair one in Settings › Bluetooth (touch controls come later)."
        }
    }

    @objc private func play() {
        guard mcla_game_available() != 0, GameFolder.locate() != nil else { refresh(); return }
        LaunchSettings.applyToGame()
        let vc = GameViewController(stage: 5)
        vc.modalPresentationStyle = .fullScreen
        present(vc, animated: true)
    }

    @objc private func openSettings() {
        presentSheet(SettingsViewController(launcher: self))
    }

    @objc private func openGameFiles() {
        presentSheet(GameFilesViewController(launcher: self))
    }

    @objc private func openLog() {
        presentSheet(MainViewController())
    }

    func presentSheet(_ root: UIViewController) {
        let nav = UINavigationController(rootViewController: root)
        nav.overrideUserInterfaceStyle = .dark
        nav.modalPresentationStyle = .formSheet
        nav.preferredContentSize = CGSize(width: 720, height: 820)
        present(nav, animated: true)
    }
}

// MARK: - Settings

final class SettingsViewController: UITableViewController {
    private weak var launcher: LauncherViewController?
    private let rows: [(symbol: String, title: String, detail: String)] = [
        ("display", "Graphics", "Frame rate, resolution, effects"),
        ("gamecontroller", "Controls", "Controllers and buttons"),
        ("folder", "Game files", "Where the game is, importing a folder"),
        ("ipad", "This device", "Memory, heat, logs and developer tools"),
    ]

    init(launcher: LauncherViewController) {
        self.launcher = launcher
        super.init(style: .insetGrouped)
    }

    required init?(coder: NSCoder) { fatalError() }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Settings"
        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .done, target: self,
                                                            action: #selector(done))
        navigationController?.navigationBar.tintColor = Theme.accent
    }

    @objc private func done() {
        dismiss(animated: true) { [weak self] in self?.launcher?.refresh() }
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int { rows.count }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        return "MCLA Port v\(version) · \(mcla_game_available() != 0 ? "game build" : "public build")"
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .subtitle, reuseIdentifier: nil)
        let row = rows[indexPath.row]
        var content = cell.defaultContentConfiguration()
        content.image = UIImage(systemName: row.symbol)
        content.imageProperties.tintColor = Theme.accent
        content.text = row.title
        content.textProperties.font = .systemFont(ofSize: 19, weight: .semibold)
        content.secondaryText = row.detail
        content.secondaryTextProperties.color = .secondaryLabel
        content.directionalLayoutMargins = NSDirectionalEdgeInsets(top: 12, leading: 0, bottom: 12, trailing: 0)
        cell.contentConfiguration = content
        cell.accessoryType = .disclosureIndicator
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let next: UIViewController
        switch indexPath.row {
        case 0: next = GraphicsSettingsViewController()
        case 1: next = ControlsViewController()
        case 2: next = GameFilesViewController(launcher: launcher)
        default: next = MainViewController()
        }
        navigationController?.pushViewController(next, animated: true)
    }
}

final class GraphicsSettingsViewController: UITableViewController {
    init() { super.init(style: .insetGrouped) }
    required init?(coder: NSCoder) { fatalError() }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Graphics"
    }

    override func numberOfSections(in tableView: UITableView) -> Int { LaunchSettings.choices.count + 1 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        section < LaunchSettings.choices.count ? 1 : LaunchSettings.toggles.count
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        section < LaunchSettings.choices.count ? LaunchSettings.choices[section].title : "Effects"
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        section < LaunchSettings.choices.count
            ? LaunchSettings.choices[section].detail
            : "Applied the next time you press Play. Turning effects off makes the game smoother on this iPad."
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .subtitle, reuseIdentifier: nil)
        cell.selectionStyle = .none
        if indexPath.section < LaunchSettings.choices.count {
            let choice = LaunchSettings.choices[indexPath.section]
            let control = UISegmentedControl(items: choice.options.map { $0.label })
            control.selectedSegmentIndex = choice.options.firstIndex { $0.value == choice.value } ?? 0
            control.selectedSegmentTintColor = Theme.accent
            control.tag = indexPath.section
            control.addTarget(self, action: #selector(choiceChanged(_:)), for: .valueChanged)
            control.translatesAutoresizingMaskIntoConstraints = false
            cell.contentView.addSubview(control)
            NSLayoutConstraint.activate([
                control.leadingAnchor.constraint(equalTo: cell.contentView.layoutMarginsGuide.leadingAnchor),
                control.trailingAnchor.constraint(equalTo: cell.contentView.layoutMarginsGuide.trailingAnchor),
                control.topAnchor.constraint(equalTo: cell.contentView.topAnchor, constant: 10),
                control.bottomAnchor.constraint(equalTo: cell.contentView.bottomAnchor, constant: -10),
                control.heightAnchor.constraint(equalToConstant: 36),
            ])
        } else {
            let toggle = LaunchSettings.toggles[indexPath.row]
            var content = cell.defaultContentConfiguration()
            content.text = toggle.title
            content.secondaryText = toggle.detail.isEmpty ? nil : toggle.detail
            content.secondaryTextProperties.color = .secondaryLabel
            cell.contentConfiguration = content
            let toggleSwitch = UISwitch()
            toggleSwitch.isOn = toggle.isOn
            toggleSwitch.onTintColor = Theme.accent
            toggleSwitch.tag = indexPath.row
            toggleSwitch.addTarget(self, action: #selector(toggleChanged(_:)), for: .valueChanged)
            cell.accessoryView = toggleSwitch
        }
        return cell
    }

    @objc private func choiceChanged(_ sender: UISegmentedControl) {
        let choice = LaunchSettings.choices[sender.tag]
        choice.value = choice.options[sender.selectedSegmentIndex].value
    }

    @objc private func toggleChanged(_ sender: UISwitch) {
        LaunchSettings.toggles[sender.tag].isOn = sender.isOn
    }
}

final class ControlsViewController: UITableViewController {
    init() { super.init(style: .insetGrouped) }
    required init?(coder: NSCoder) { fatalError() }

    private var pads: [GCController] { GCController.controllers().filter { $0.extendedGamepad != nil } }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Controls"
        NotificationCenter.default.addObserver(self, selector: #selector(reload),
                                               name: .GCControllerDidConnect, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(reload),
                                               name: .GCControllerDidDisconnect, object: nil)
    }

    @objc private func reload() { tableView.reloadData() }

    override func numberOfSections(in tableView: UITableView) -> Int { 2 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        section == 0 ? max(pads.count, 1) : Self.mapping.count
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        section == 0 ? "Controllers" : "Buttons (as on an Xbox 360 pad)"
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        section == 0
            ? "Xbox, PlayStation and MFi controllers paired in Settings › Bluetooth work. Touch controls come later."
            : nil
    }

    private static let mapping: [(String, String)] = [
        ("A / Cross", "A"), ("B / Circle", "B"), ("X / Square", "X"), ("Y / Triangle", "Y"),
        ("Menu (≡) / Options", "Start"), ("View (⧉) / Share", "Back"),
        ("Bumpers, triggers, sticks, d-pad", "Same as on the Xbox 360 pad"),
    ]

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .value1, reuseIdentifier: nil)
        cell.selectionStyle = .none
        var content = UIListContentConfiguration.valueCell()
        if indexPath.section == 0 {
            if let pad = pads[safe: indexPath.row] {
                content.image = UIImage(systemName: "gamecontroller.fill")
                content.imageProperties.tintColor = Theme.accent
                content.text = pad.vendorName ?? "Controller"
                content.secondaryText = pad.productCategory
            } else {
                content.image = UIImage(systemName: "gamecontroller")
                content.text = "No controller connected"
            }
        } else {
            content.text = Self.mapping[indexPath.row].0
            content.secondaryText = Self.mapping[indexPath.row].1
        }
        cell.contentConfiguration = content
        return cell
    }
}

final class GameFilesViewController: UITableViewController, UIDocumentPickerDelegate {
    private weak var launcher: LauncherViewController?

    init(launcher: LauncherViewController?) {
        self.launcher = launcher
        super.init(style: .insetGrouped)
    }

    required init?(coder: NSCoder) { fatalError() }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Game files"
        if navigationController?.viewControllers.first === self {
            navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .done, target: self,
                                                                action: #selector(done))
        }
    }

    @objc private func done() {
        dismiss(animated: true) { [weak self] in self?.launcher?.refresh() }
    }

    override func numberOfSections(in tableView: UITableView) -> Int { 2 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        section == 0 ? 1 : 2
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        section == 0
            ? "Put the whole game folder (default.xex and the .rpf and .bik files) in On My iPad › MCLA › game, using the Files app, LocalSend or Apple Devices on the PC. Keep the app installed when updating: deleting it deletes these files."
            : "Importing a folder elsewhere works too; the app remembers where it is."
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .subtitle, reuseIdentifier: nil)
        var content = cell.defaultContentConfiguration()
        content.secondaryTextProperties.color = .secondaryLabel
        if indexPath.section == 0 {
            cell.selectionStyle = .none
            if let folder = GameFolder.locate() {
                content.image = UIImage(systemName: "checkmark.circle.fill")
                content.imageProperties.tintColor = .systemGreen
                content.text = "default.xex found"
                content.secondaryText = folder.path.replacingOccurrences(
                    of: FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path,
                    with: "On My iPad › MCLA")
            } else {
                content.image = UIImage(systemName: "exclamationmark.circle.fill")
                content.imageProperties.tintColor = .systemOrange
                content.text = "Game files not found"
                content.secondaryText = "No default.xex in On My iPad › MCLA › game"
            }
        } else if indexPath.row == 0 {
            content.image = UIImage(systemName: "arrow.clockwise")
            content.imageProperties.tintColor = Theme.accent
            content.text = "Check again"
        } else {
            content.image = UIImage(systemName: "folder.badge.plus")
            content.imageProperties.tintColor = Theme.accent
            content.text = "Import a game folder…"
        }
        cell.contentConfiguration = content
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        guard indexPath.section == 1 else { return }
        if indexPath.row == 0 {
            tableView.reloadData()
            launcher?.refresh()
        } else {
            let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder])
            picker.delegate = self
            present(picker, animated: true)
        }
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let url = urls.first else { return }
        do {
            let bookmark = try url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
            UserDefaults.standard.set(bookmark, forKey: GameFolder.bookmarkKey)
            log("picked game folder: \(url.lastPathComponent)")
        } catch {
            log("could not remember the folder: \(error.localizedDescription)")
        }
        tableView.reloadData()
        launcher?.refresh()
    }
}

private extension Array {
    subscript(safe index: Int) -> Element? { indices.contains(index) ? self[index] : nil }
}
