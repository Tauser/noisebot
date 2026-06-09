import { Download, RefreshCw } from "lucide-react";
import type { AudioSampleFile } from "../api";
import { audioSampleDownloadUrl } from "../api";
import { bytesValue } from "../lib/format";
import { secondaryButtonClass, iconButtonClass } from "../lib/classes";

export function AudioSamplesPanel({
  files,
  loading,
  onRefresh,
  status,
}: {
  files: AudioSampleFile[];
  loading: boolean;
  onRefresh: () => void;
  status: string;
}) {
  return (
    <div className="mt-4 border-t border-white/10 pt-4">
      <div className="mb-3 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h3 className="text-sm font-bold text-slate-100">Amostras de áudio</h3>
          <p className="text-sm text-slate-400">{status}</p>
        </div>
        <button
          className={secondaryButtonClass}
          disabled={loading}
          onClick={onRefresh}
          type="button"
        >
          <RefreshCw size={16} />
          {loading ? "Listando" : "Atualizar"}
        </button>
      </div>
      {files.length === 0 ? (
        <p className="rounded-lg border border-dashed border-white/10 p-3 text-sm text-slate-400">
          Nenhuma amostra carregada.
        </p>
      ) : (
        <div className="grid gap-2">
          {files.map((file) => (
            <div
              className="flex min-h-12 flex-wrap items-center justify-between gap-3 rounded-lg border border-white/10 bg-white/[0.03] px-3 py-2"
              key={file.name}
            >
              <div className="min-w-0">
                <strong className="block break-all text-sm text-slate-100">
                  {file.name}
                </strong>
                <span className="text-xs font-semibold text-slate-400">
                  {bytesValue(file.size)}
                </span>
              </div>
              <a
                className={iconButtonClass}
                href={audioSampleDownloadUrl(file.name)}
                title={`Baixar ${file.name}`}
              >
                <Download size={16} />
              </a>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
