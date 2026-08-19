import * as React from "react";
import { Anvil, BookOpen, Import, ScrollText } from "lucide-react";
import { api } from "./lib/api";
import type { Biome, Kind, LibraryEntry, RulesDoc, SpeciesRow } from "./lib/schema";
import { Button } from "./components/ui/button";
import { useToast } from "./components/ui/toast";
import { LibraryView } from "./components/LibraryView";
import { RulesView } from "./components/RulesView";
import { ImportDialog } from "./components/ImportDialog";
import { cn } from "./lib/cn";

/** Everything the app knows, fetched from the server (which reads
 * forge/biomes.py, forge/kinds.py and rules/*.json -- never duplicated here). */
export interface World {
  biomes: Biome[];
  kinds: Kind[];
  specs: SpeciesRow[];
  library: LibraryEntry[];
  rules: RulesDoc;
  palette: Record<string, [number, number, number]>;
  refreshSpecs: () => Promise<void>;
  refreshLibrary: () => Promise<void>;
  refreshRules: () => Promise<void>;
  patchSpec: (name: string, patch: Partial<SpeciesRow>) => void;
}

export default function App() {
  const toast = useToast();
  const [tab, setTab] = React.useState<"library" | "rules">("library");
  const [importOpen, setImportOpen] = React.useState(false);
  const [world, setWorld] = React.useState<World | null>(null);
  const [bootError, setBootError] = React.useState<string | null>(null);

  const load = React.useCallback(async () => {
    try {
      const [biomes, kinds, specs, library, rules, palette] = await Promise.all([
        api.biomes(), api.kinds(), api.specs(), api.library(), api.rules(), api.palette(),
      ]);
      setWorld({
        biomes, kinds, specs, library, rules, palette,
        refreshSpecs: async () => {
          const s = await api.specs();
          setWorld((w) => (w ? { ...w, specs: s } : w));
        },
        refreshLibrary: async () => {
          const l = await api.library();
          setWorld((w) => (w ? { ...w, library: l } : w));
        },
        refreshRules: async () => {
          const r = await api.rules();
          setWorld((w) => (w ? { ...w, rules: r } : w));
        },
        patchSpec: (name, patch) =>
          setWorld((w) =>
            w ? { ...w, specs: w.specs.map((s) => (s.name === name ? { ...s, ...patch } : s)) } : w),
      });
    } catch (e) {
      setBootError(String(e));
    }
  }, []);

  React.useEffect(() => {
    void load();
  }, [load]);

  if (bootError)
    return (
      <div className="flex h-full items-center justify-center p-8">
        <div className="chamfer bevel-up max-w-lg bg-stone-800 p-6 text-sm">
          <div className="mb-2 font-display text-lg text-rust-400">The forge is cold</div>
          <p className="text-parch-300">
            Could not reach the server: {bootError}. Start it with{" "}
            <code className="font-mono text-parch-100">python -m forge.cli serve</code> and reload.
          </p>
        </div>
      </div>
    );

  return (
    <div className="flex h-full flex-col">
      <header className="mortar-b flex items-center gap-4 bg-stone-850 px-4 py-2">
        <div className="flex items-center gap-2 font-display text-xl tracking-widest text-gold-400">
          <Anvil className="h-6 w-6" />
          <span className="uppercase">Asset Forge</span>
        </div>
        <nav className="ml-6 flex gap-1">
          <TabButton active={tab === "library"} onClick={() => setTab("library")}>
            <BookOpen className="h-4 w-4" /> Library
          </TabButton>
          <TabButton active={tab === "rules"} onClick={() => setTab("rules")}>
            <ScrollText className="h-4 w-4" /> Placement rules
          </TabButton>
        </nav>
        <div className="ml-auto flex items-center gap-3">
          {world && (
            <span className="font-mono text-xs text-parch-500">
              {world.specs.length} species · {world.library.length} kept variants ·{" "}
              {Object.keys(world.rules.rules).length} rules
            </span>
          )}
          <Button variant="gold" size="sm" onClick={() => setImportOpen(true)}>
            <Import className="h-4 w-4" /> Import asset
          </Button>
        </div>
      </header>

      {!world ? (
        <div className="flex flex-1 items-center justify-center font-display text-parch-400">
          Stoking the forge…
        </div>
      ) : tab === "library" ? (
        <LibraryView world={world} />
      ) : (
        <RulesView world={world} />
      )}

      {world && (
        <ImportDialog
          open={importOpen}
          onOpenChange={setImportOpen}
          world={world}
          onImported={async (entry) => {
            toast.ok("Imported " + entry.id + " into the library");
            await Promise.all([world.refreshLibrary(), world.refreshSpecs()]);
          }}
        />
      )}
    </div>
  );
}

function TabButton({
  active, onClick, children,
}: {
  active: boolean;
  onClick: () => void;
  children: React.ReactNode;
}) {
  return (
    <button
      onClick={onClick}
      className={cn(
        "chamfer-sm flex items-center gap-1.5 px-3 py-1.5 font-display text-sm tracking-wide transition-colors",
        active ? "bevel-up bg-stone-600 text-gold-400" : "text-parch-400 hover:bg-stone-700 hover:text-parch-200",
      )}
    >
      {children}
    </button>
  );
}
