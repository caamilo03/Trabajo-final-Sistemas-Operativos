#!/usr/bin/env python3
"""
analyze.py — Análisis estadístico del experimento perfanalyzer.

Lee experiments/results/raw_results.csv y produce:
  - Estadística descriptiva por celda
  - Tests de normalidad (Shapiro-Wilk)
  - Comparación host vs Docker (Welch t-test / Mann-Whitney)
  - Tamaño del efecto (Cohen's d / r de rango)
  - Figuras: boxplots y ECDF
  - Reporte HTML: experiments/results/report.html
"""

import sys
import os
import math
import warnings
from pathlib import Path

import numpy as np
import pandas as pd
import scipy.stats as stats
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import seaborn as sns

RESULTS_DIR = Path(__file__).parent / "results"
CSV_FILE    = RESULTS_DIR / "raw_results.csv"
HTML_OUT    = RESULTS_DIR / "report.html"
FIG_DIR     = RESULTS_DIR / "figures"
FIG_DIR.mkdir(exist_ok=True)

ALPHA = 0.05   # nivel de significancia
N_BOOTSTRAP = 2000


# ── Utilidades estadísticas ──────────────────────────────────────────────────

def bootstrap_ci(data: np.ndarray, stat=np.mean, n=N_BOOTSTRAP, ci=0.95) -> tuple:
    """Intervalo de confianza por bootstrap."""
    rng = np.random.default_rng(42)
    samples = [stat(rng.choice(data, size=len(data), replace=True)) for _ in range(n)]
    lo = (1 - ci) / 2
    hi = 1 - lo
    return float(np.quantile(samples, lo)), float(np.quantile(samples, hi))


def cohens_d(a: np.ndarray, b: np.ndarray) -> float:
    """Cohen's d: (mu_a - mu_b) / pooled_std."""
    n1, n2 = len(a), len(b)
    if n1 < 2 or n2 < 2:
        return float("nan")
    pooled_std = math.sqrt(
        ((n1 - 1) * float(np.var(a, ddof=1)) + (n2 - 1) * float(np.var(b, ddof=1)))
        / (n1 + n2 - 2)
    )
    return (float(np.mean(a)) - float(np.mean(b))) / pooled_std if pooled_std > 0 else float("nan")


def rank_biserial_r(a: np.ndarray, b: np.ndarray) -> float:
    """Tamaño del efecto r para Mann-Whitney."""
    u_stat, _ = stats.mannwhitneyu(a, b, alternative="two-sided")
    return 1 - (2 * u_stat) / (len(a) * len(b))


def compare(a: np.ndarray, b: np.ndarray, label_a: str, label_b: str) -> dict:
    """Compara dos muestras, elige el test adecuado según normalidad."""
    result = {"label_a": label_a, "label_b": label_b,
              "n_a": len(a), "n_b": len(b),
              "mean_a": float(np.mean(a)), "mean_b": float(np.mean(b)),
              "std_a": float(np.std(a, ddof=1)), "std_b": float(np.std(b, ddof=1))}

    ci_a = bootstrap_ci(a)
    ci_b = bootstrap_ci(b)
    result["ci95_a"] = ci_a
    result["ci95_b"] = ci_b

    _, p_norm_a = stats.shapiro(a[:50]) if len(a) > 3 else (0, 0)
    _, p_norm_b = stats.shapiro(b[:50]) if len(b) > 3 else (0, 0)
    result["p_shapiro_a"] = float(p_norm_a)
    result["p_shapiro_b"] = float(p_norm_b)

    both_normal = (p_norm_a > ALPHA and p_norm_b > ALPHA)
    if both_normal:
        t_stat, p_val = stats.ttest_ind(a, b, equal_var=False)
        result["test"] = "Welch t-test"
        result["statistic"] = float(t_stat)
        result["effect_size"] = cohens_d(a, b)
        result["effect_label"] = "Cohen's d"
    else:
        u_stat, p_val = stats.mannwhitneyu(a, b, alternative="two-sided")
        result["test"] = "Mann-Whitney U"
        result["statistic"] = float(u_stat)
        result["effect_size"] = rank_biserial_r(a, b)
        result["effect_label"] = "rank-biserial r"

    result["p_value"] = float(p_val)
    result["significant"] = bool(p_val < ALPHA)
    return result


# ── Figuras ──────────────────────────────────────────────────────────────────

def boxplot(df: pd.DataFrame, metric: str, title: str, fname: str):
    fig, ax = plt.subplots(figsize=(12, 5))
    order = sorted(df["env_profile"].unique())
    sns.boxplot(data=df, x="env_profile", y=metric, hue="workload",
                order=order, ax=ax, flierprops=dict(marker=".", markersize=3))
    ax.set_title(title)
    ax.set_xlabel("Entorno")
    ax.set_ylabel(metric)
    ax.tick_params(axis="x", rotation=30)
    plt.tight_layout()
    path = FIG_DIR / fname
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return str(path.relative_to(RESULTS_DIR))


def ecdf_plot(df: pd.DataFrame, metric: str, env1: str, env2: str,
              workload: str, fname: str):
    fig, ax = plt.subplots(figsize=(7, 4))
    for env, color in [(env1, "steelblue"), (env2, "tomato")]:
        d = df[(df["env_profile"] == env) & (df["workload"] == workload)][metric].dropna()
        if len(d) == 0:
            continue
        x = np.sort(d.values)
        y = np.arange(1, len(x) + 1) / len(x)
        ax.step(x, y, where="post", color=color, label=env)
    ax.set_xlabel(metric)
    ax.set_ylabel("F(x)")
    ax.set_title(f"ECDF — {metric} | {workload}")
    ax.legend()
    plt.tight_layout()
    path = FIG_DIR / fname
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return str(path.relative_to(RESULTS_DIR))


# ── Reporte HTML ─────────────────────────────────────────────────────────────

def fmt_ci(ci: tuple) -> str:
    return f"[{ci[0]:.2f}, {ci[1]:.2f}]"


def comparison_table_html(rows: list) -> str:
    cols = ["Métrica", "Workload", "Grupo A", "Grupo B",
            "Media A", "IC95 A", "Media B", "IC95 B",
            "Test", "p-valor", "Sig.", "Efecto", "Val. efecto"]
    header = "".join(f"<th>{c}</th>" for c in cols)
    body = ""
    for r in rows:
        sig = "✓" if r["significant"] else "✗"
        body += (
            f"<tr>"
            f"<td>{r.get('metric','')}</td>"
            f"<td>{r.get('workload','')}</td>"
            f"<td>{r['label_a']}</td>"
            f"<td>{r['label_b']}</td>"
            f"<td>{r['mean_a']:.2f}</td>"
            f"<td>{fmt_ci(r['ci95_a'])}</td>"
            f"<td>{r['mean_b']:.2f}</td>"
            f"<td>{fmt_ci(r['ci95_b'])}</td>"
            f"<td>{r['test']}</td>"
            f"<td>{r['p_value']:.4f}</td>"
            f"<td>{sig}</td>"
            f"<td>{r['effect_label']}</td>"
            f"<td>{r['effect_size']:.3f}</td>"
            f"</tr>"
        )
    return f"<table border='1' cellpadding='4'><thead><tr>{header}</tr></thead><tbody>{body}</tbody></table>"


def figure_html(figures: list) -> str:
    html = ""
    for title, rel_path in figures:
        html += (f"<figure>"
                 f"<img src='{rel_path}' style='max-width:100%;'>"
                 f"<figcaption>{title}</figcaption>"
                 f"</figure>")
    return html


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    if not CSV_FILE.exists():
        print(f"ERROR: No se encontró {CSV_FILE}")
        print("Ejecuta experiments/run_experiment.sh primero.")
        sys.exit(1)

    df = pd.read_csv(CSV_FILE)
    print(f"Filas cargadas: {len(df)}")
    print(df.dtypes)

    # Columna combinada env_profile
    df["env_profile"] = df.apply(
        lambda r: "host" if r["env"] == "host" else f"docker:{r['profile']}", axis=1
    )

    METRICS = {
        "cpu_pct_avg": "CPU promedio (%)",
        "mem_rss_avg_kb": "RSS promedio (kB)",
        "probe_p99_us": "Latencia p99 de sonda (µs)",
        "throughput": "Throughput (iteraciones)",
    }

    # ── Estadística descriptiva ──────────────────────────────────────────────
    desc = df.groupby(["env_profile", "workload", "intensity"])[list(METRICS.keys())].describe()
    print("\n=== Estadística descriptiva ===")
    print(desc.to_string())

    # ── Figuras ──────────────────────────────────────────────────────────────
    figures = []
    for metric, label in METRICS.items():
        fname = f"boxplot_{metric}.png"
        rel = boxplot(df, metric, f"{label} por entorno", fname)
        figures.append((f"Figura: {label}", rel))

    # ECDF host vs docker:baseline para cpu_pct_avg
    for wl in df["workload"].unique():
        for env2 in ["docker:baseline", "docker:cpu-limit", "docker:mem-limit"]:
            if env2 not in df["env_profile"].values:
                continue
            fname = f"ecdf_cpu_{wl}_{env2.replace(':','_')}.png"
            rel = ecdf_plot(df, "cpu_pct_avg", "host", env2, wl, fname)
            figures.append((f"ECDF CPU — {wl} — host vs {env2}", rel))

    # ── Comparaciones estadísticas ───────────────────────────────────────────
    comparison_rows = []
    host_df = df[df["env"] == "host"]

    for metric in METRICS:
        for wl in df["workload"].unique():
            for inten in df["intensity"].unique():
                a = host_df[(host_df["workload"] == wl) &
                             (host_df["intensity"] == inten)][metric].dropna().values
                if len(a) < 5:
                    continue
                for profile in df[df["env"] == "docker"]["profile"].unique():
                    b = df[(df["env"] == "docker") & (df["profile"] == profile) &
                           (df["workload"] == wl) &
                           (df["intensity"] == inten)][metric].dropna().values
                    if len(b) < 5:
                        continue
                    row = compare(a, b, "host", f"docker:{profile}")
                    row["metric"]    = metric
                    row["workload"]  = f"{wl}:inten{inten}"
                    comparison_rows.append(row)

    # ── Reporte HTML ─────────────────────────────────────────────────────────
    desc_html = desc.to_html(classes="desc-table", border=1, float_format=lambda x: f"{x:.2f}")
    comp_html = comparison_table_html(comparison_rows)
    figs_html = figure_html(figures)

    html = f"""<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<title>Reporte perfanalyzer</title>
<style>
  body {{ font-family: sans-serif; margin: 2em; }}
  table {{ border-collapse: collapse; font-size: 0.85em; }}
  th {{ background: #ddd; }}
  td, th {{ padding: 4px 8px; }}
  figure {{ display: inline-block; margin: 1em; vertical-align: top; }}
  figcaption {{ text-align: center; font-size: 0.8em; }}
  h2 {{ border-bottom: 2px solid #333; }}
</style>
</head>
<body>
<h1>Reporte de Experimentos — perfanalyzer</h1>
<p>Generado: <code>{pd.Timestamp.now().isoformat()}</code> | α={ALPHA} | Bootstrap n={N_BOOTSTRAP}</p>

<h2>1. Estadística Descriptiva</h2>
{desc_html}

<h2>2. Comparaciones Estadísticas (Host vs. Docker)</h2>
<p>✓ = diferencia estadísticamente significativa (p &lt; {ALPHA})</p>
{comp_html}

<h2>3. Figuras</h2>
{figs_html}
</body>
</html>"""

    HTML_OUT.write_text(html, encoding="utf-8")
    print(f"\nReporte generado: {HTML_OUT}")
    print(f"Figuras en: {FIG_DIR}")


if __name__ == "__main__":
    warnings.filterwarnings("ignore", category=RuntimeWarning)
    main()
