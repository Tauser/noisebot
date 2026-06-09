import { useState } from "react";
import { Bell, CheckCircle2, Clock3, Pause, Timer, Trash2 } from "lucide-react";
import type { AppData, RoutineItem } from "../api";
import { cardClass, primaryButtonClass, inputClass } from "../lib/classes";
import { kindLabel, clampNumber } from "../lib/format";

function NumberInput({
  onChange,
  value,
}: {
  onChange: (value: number) => void;
  value: number;
}) {
  return (
    <input
      className={inputClass}
      min="1"
      max="1440"
      onChange={(e) => onChange(clampNumber(e.target.value, 1, 1440))}
      type="number"
      value={value}
    />
  );
}

function RoutineForm({
  children,
  icon: Icon,
  onSubmit,
  title,
}: {
  children: React.ReactNode;
  icon: typeof Timer;
  onSubmit: () => void;
  title: string;
}) {
  return (
    <section className={cardClass}>
      <h2 className="mb-3 flex items-center gap-2 text-base font-semibold">
        <Icon size={18} /> {title}
      </h2>
      <form
        className="grid gap-3"
        onSubmit={(e) => {
          e.preventDefault();
          onSubmit();
        }}
      >
        {children}
        <button className={primaryButtonClass} type="submit">
          Criar
        </button>
      </form>
    </section>
  );
}

export function RoutineView({
  items,
  onCreateAlarm,
  onCreateReminder,
  onCreateTimer,
  onRemove,
  onToggle,
  status,
  summary,
}: {
  items: RoutineItem[];
  onCreateAlarm: (title: string, time: string, repeat: string) => void;
  onCreateReminder: (title: string, durationMin: number) => void;
  onCreateTimer: (title: string, durationMin: number) => void;
  onRemove: (item: RoutineItem) => void;
  onToggle: (item: RoutineItem) => void;
  status: string;
  summary: AppData["routine"]["summary"];
}) {
  const [timerTitle, setTimerTitle] = useState("Timer");
  const [timerMin, setTimerMin] = useState(10);
  const [alarmTitle, setAlarmTitle] = useState("Alarme");
  const [alarmTime, setAlarmTime] = useState("07:30");
  const [alarmRepeat, setAlarmRepeat] = useState("diário");
  const [reminderTitle, setReminderTitle] = useState("Lembrete");
  const [reminderMin, setReminderMin] = useState(15);

  return (
    <div className="grid gap-4">
      <section className={cardClass}>
        <div className="flex flex-wrap items-start justify-between gap-4">
          <div>
            <h2 className="text-lg font-semibold">Rotinas e produtividade</h2>
            <p className="text-sm text-slate-400">{summary.next}</p>
          </div>
          {status !== "pronto" && (
            <span className="text-sm font-medium text-slate-400">{status}</span>
          )}
        </div>
      </section>

      <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_300px]">
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Itens</h2>
          <div className="grid gap-2">
            {items.length === 0 ? (
              <p className="rounded-lg bg-black/[0.10] p-4 text-sm text-slate-400">
                Nenhum item criado.
              </p>
            ) : (
              items.map((item) => (
                <article
                  className="grid grid-cols-[auto_minmax(0,1fr)_auto] items-center gap-3 rounded-lg bg-black/[0.15] p-3"
                  key={item.id}
                >
                  <button
                    className={
                      item.enabled
                        ? "inline-flex h-9 w-9 items-center justify-center rounded-lg bg-emerald-400/10 text-emerald-300"
                        : "inline-flex h-9 w-9 items-center justify-center rounded-lg bg-black/[0.20] text-slate-400"
                    }
                    onClick={() => onToggle(item)}
                    type="button"
                  >
                    {item.enabled ? <CheckCircle2 size={16} /> : <Pause size={16} />}
                  </button>
                  <div className="min-w-0">
                    <strong className="block truncate">{item.title}</strong>
                    <span className="text-sm text-slate-400">
                      {kindLabel(item.kind)} · {item.detail || item.status}
                    </span>
                  </div>
                  <button
                    className="inline-flex h-9 w-9 items-center justify-center rounded-lg bg-black/[0.15] text-slate-400 hover:bg-black/[0.25] hover:text-white"
                    onClick={() => onRemove(item)}
                    type="button"
                  >
                    <Trash2 size={17} />
                  </button>
                </article>
              ))
            )}
          </div>
        </section>

        <aside className="grid content-start gap-4">
          <RoutineForm
            icon={Timer}
            title="Novo timer"
            onSubmit={() => onCreateTimer(timerTitle, timerMin)}
          >
            <input
              className={inputClass}
              onChange={(e) => setTimerTitle(e.target.value)}
              value={timerTitle}
            />
            <NumberInput onChange={setTimerMin} value={timerMin} />
          </RoutineForm>
          <RoutineForm
            icon={Bell}
            title="Novo alarme"
            onSubmit={() => onCreateAlarm(alarmTitle, alarmTime, alarmRepeat)}
          >
            <input
              className={inputClass}
              onChange={(e) => setAlarmTitle(e.target.value)}
              value={alarmTitle}
            />
            <input
              className={inputClass}
              onChange={(e) => setAlarmTime(e.target.value)}
              type="time"
              value={alarmTime}
            />
            <select
              className={inputClass}
              onChange={(e) => setAlarmRepeat(e.target.value)}
              value={alarmRepeat}
            >
              <option value="diário">Diário</option>
              <option value="dias úteis">Dias úteis</option>
              <option value="fim de semana">Fim de semana</option>
              <option value="uma vez">Uma vez</option>
            </select>
          </RoutineForm>
          <RoutineForm
            icon={Clock3}
            title="Novo lembrete"
            onSubmit={() => onCreateReminder(reminderTitle, reminderMin)}
          >
            <input
              className={inputClass}
              onChange={(e) => setReminderTitle(e.target.value)}
              value={reminderTitle}
            />
            <NumberInput onChange={setReminderMin} value={reminderMin} />
          </RoutineForm>
        </aside>
      </div>
    </div>
  );
}
