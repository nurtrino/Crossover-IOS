import SwiftUI

struct NewBottleWizard: View {
    @EnvironmentObject private var store: BottleStore
    @Environment(\.dismiss) private var dismiss

    @State private var name = ""
    @State private var guestType: Bottle.GuestType = .linuxWine

    var body: some View {
        NavigationStack {
            Form {
                Section("Bottle") {
                    TextField("Name (e.g. Notepad++)", text: $name)
                }
                Section {
                    Picker("Environment", selection: $guestType) {
                        Text("Linux + Wine (recommended)").tag(Bottle.GuestType.linuxWine)
                        Text("Windows ARM (advanced)").tag(Bottle.GuestType.windowsArm)
                    }
                    .pickerStyle(.inline)
                } footer: {
                    Text("The built-in environment runs most Windows applications without any OS image. Windows ARM bottles require an image you provide.")
                }
            }
            .navigationTitle("New Bottle")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") {
                        _ = store.createBottle(name: name.trimmingCharacters(in: .whitespaces), guestType: guestType)
                        dismiss()
                    }
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }
}

#Preview {
    NewBottleWizard()
        .environmentObject(BottleStore(preview: true))
}
