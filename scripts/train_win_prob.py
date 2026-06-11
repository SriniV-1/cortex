#!/usr/bin/env python3
"""
Train an expanded win probability model with team Elo features.

Features (7):
  [0] score_diff       — home_score - away_score (raw)
  [1] quarter          — period (1-4, 5+ for OT)
  [2] sec_remaining    — total seconds left in game
  [3] home_advantage   — always 1.0 (we're predicting from home perspective)
  [4] momentum         — placeholder (0.0 — filled at inference time)
  [5] elo_diff         — home_elo - away_elo (from team_elo table)
  [6] elo_expected     — Elo expected win prob (logistic of elo_diff/400)

Label: 1 if home team won, 0 if away team won.

Training data: sampled game states from play_events joined with game outcomes
and team Elo ratings. Each sample is a snapshot at some point during the game.

Output: data/models/win_prob.onnx (replaces the old 5-feature model)
"""

import os
import sys
import numpy as np
import psycopg2
from sklearn.linear_model import LogisticRegression
from sklearn.model_selection import StratifiedKFold
from sklearn.calibration import calibration_curve
from sklearn.metrics import accuracy_score, log_loss, roc_auc_score
import onnx

DB_CONN = os.environ.get("CORTEX_DB", "host=localhost port=5433 dbname=cortex")
MODEL_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "models", "win_prob.onnx")
NUM_FEATURES = 7


def elo_expected(home_elo: float, away_elo: float, home_adv: float = 100.0) -> float:
    """Standard Elo expected score with home advantage."""
    diff = (home_elo + home_adv) - away_elo
    return 1.0 / (1.0 + 10.0 ** (-diff / 400.0))


def fetch_training_data(conn) -> tuple:
    """
    Sample game states from play_events.
    For each game, sample ~5 events spread across the game.
    Join with game outcome (home_won) and team Elo ratings.
    """
    cur = conn.cursor()

    # Get Elo ratings
    cur.execute("SELECT team_id, rating FROM team_elo")
    elo_map = {row[0]: row[1] for row in cur.fetchall()}
    default_elo = 1500.0

    # Get completed games with scores
    cur.execute("""
        SELECT g.game_id, g.home_team_id, g.away_team_id,
               g.home_score AS final_home, g.away_score AS final_away
        FROM games g
        WHERE g.status = 3 AND g.home_score IS NOT NULL
          AND g.home_score != g.away_score
    """)
    games = cur.fetchall()
    print(f"Found {len(games)} completed games")

    X_rows = []
    y_rows = []

    for i, (game_id, home_tid, away_tid, final_home, final_away) in enumerate(games):
        home_won = 1 if final_home > final_away else 0
        home_elo = elo_map.get(home_tid, default_elo)
        away_elo = elo_map.get(away_tid, default_elo)
        elo_d = home_elo - away_elo
        elo_exp = elo_expected(home_elo, away_elo)

        # Sample up to 5 events from this game (spread across the game)
        cur.execute("""
            SELECT pe.period, pe.score_home, pe.score_away, pe.action_number
            FROM play_events pe
            WHERE pe.game_id = %s AND pe.score_home IS NOT NULL
            ORDER BY pe.action_number ASC
        """, (game_id,))
        events = cur.fetchall()

        if len(events) < 10:
            continue

        # Sample 5 evenly spaced events
        indices = np.linspace(0, len(events) - 1, 5, dtype=int)
        for idx in indices:
            period, score_home, score_away, action_num = events[idx]
            score_diff = float(score_home - score_away)
            quarter = float(min(period, 5))

            # Estimate seconds remaining:
            # Rough: each period is 720s, fraction through game based on action progress
            total_actions = len(events)
            fraction_done = idx / total_actions
            # 4 periods of 720s = 2880s total
            sec_remaining = max(0.0, 2880.0 * (1.0 - fraction_done))

            X_rows.append([
                score_diff,
                quarter,
                sec_remaining,
                1.0,          # home_advantage
                0.0,          # momentum (placeholder)
                elo_d,        # elo_diff
                elo_exp,      # elo_expected
            ])
            y_rows.append(home_won)

        if (i + 1) % 1000 == 0:
            print(f"  Processed {i + 1}/{len(games)} games…")

    cur.close()
    return np.array(X_rows, dtype=np.float32), np.array(y_rows, dtype=np.int32)


FEATURE_NAMES = [
    "score_diff", "quarter", "sec_remaining",
    "home_advantage", "momentum", "elo_diff", "elo_expected",
]

C_CANDIDATES = [0.01, 0.1, 1.0, 10.0]


def evaluate_model(X: np.ndarray, y: np.ndarray) -> float:
    """
    Run 5-fold stratified CV with calibration analysis, feature ablation,
    and hyperparameter search. Returns the best C value.
    """
    print(f"\nTraining data: {X.shape[0]} samples, {X.shape[1]} features")
    print(f"Home win rate: {y.mean():.3f}")

    # ── Hyperparameter search via 5-fold CV ──────────────────────────
    print("\n" + "=" * 50)
    print("Hyperparameter search (5-fold stratified CV)")
    print("=" * 50)

    skf = StratifiedKFold(n_splits=5, shuffle=True, random_state=42)
    best_c = C_CANDIDATES[0]
    best_mean_auc = -1.0

    for c_val in C_CANDIDATES:
        fold_aucs = []
        for train_idx, val_idx in skf.split(X, y):
            m = LogisticRegression(max_iter=1000, C=c_val, solver="lbfgs", random_state=42)
            m.fit(X[train_idx], y[train_idx])
            prob = m.predict_proba(X[val_idx])[:, 1]
            fold_aucs.append(roc_auc_score(y[val_idx], prob))
        mean_auc = np.mean(fold_aucs)
        std_auc = np.std(fold_aucs)
        print(f"  C={c_val:<6g}  AUC = {mean_auc:.4f} +/- {std_auc:.4f}")
        if mean_auc > best_mean_auc:
            best_mean_auc = mean_auc
            best_c = c_val

    print(f"\n  Best C = {best_c}  (mean AUC = {best_mean_auc:.4f})")

    # ── Detailed 5-fold CV with best C ───────────────────────────────
    print("\n" + "=" * 50)
    print(f"Detailed 5-fold CV (C={best_c})")
    print("=" * 50)

    fold_metrics = {"acc": [], "auc": [], "logloss": [], "ece": []}
    all_val_probs = np.zeros(len(y))
    all_val_flags = np.zeros(len(y), dtype=bool)

    for fold_i, (train_idx, val_idx) in enumerate(skf.split(X, y)):
        m = LogisticRegression(max_iter=1000, C=best_c, solver="lbfgs", random_state=42)
        m.fit(X[train_idx], y[train_idx])
        pred = m.predict(X[val_idx])
        prob = m.predict_proba(X[val_idx])[:, 1]

        acc = accuracy_score(y[val_idx], pred)
        auc = roc_auc_score(y[val_idx], prob)
        ll = log_loss(y[val_idx], prob)

        # Per-fold ECE (10 bins)
        ece = _compute_ece(y[val_idx], prob, n_bins=10)

        fold_metrics["acc"].append(acc)
        fold_metrics["auc"].append(auc)
        fold_metrics["logloss"].append(ll)
        fold_metrics["ece"].append(ece)

        all_val_probs[val_idx] = prob
        all_val_flags[val_idx] = True

        print(f"  Fold {fold_i + 1}: acc={acc:.4f}  AUC={auc:.4f}  logloss={ll:.4f}  ECE={ece:.4f}")

    for metric in fold_metrics:
        vals = fold_metrics[metric]
        print(f"  {metric:>7s} mean={np.mean(vals):.4f} +/- {np.std(vals):.4f}")

    # ── Calibration analysis (aggregated across folds) ───────────────
    print("\n" + "=" * 50)
    print("Calibration analysis (reliability diagram)")
    print("=" * 50)

    assert all_val_flags.all(), "Not all samples were validated"
    fraction_pos, mean_pred = calibration_curve(y, all_val_probs, n_bins=10, strategy="uniform")

    print(f"  {'Bin':>4s}  {'Pred Mean':>10s}  {'Actual Frac':>12s}  {'Count':>6s}  {'Gap':>8s}")
    bin_edges = np.linspace(0, 1, 11)
    for i in range(len(fraction_pos)):
        lo, hi = bin_edges[i], bin_edges[i + 1]
        mask = (all_val_probs >= lo) & (all_val_probs < hi)
        count = mask.sum()
        gap = abs(fraction_pos[i] - mean_pred[i])
        print(f"  {i + 1:>4d}  {mean_pred[i]:>10.4f}  {fraction_pos[i]:>12.4f}  {count:>6d}  {gap:>8.4f}")

    overall_ece = _compute_ece(y, all_val_probs, n_bins=10)
    print(f"\n  Overall ECE = {overall_ece:.4f}")

    # ── Feature ablation ─────────────────────────────────────────────
    print("\n" + "=" * 50)
    print("Feature ablation (drop-one, 5-fold CV AUC)")
    print("=" * 50)

    baseline_auc = best_mean_auc
    print(f"  {'Feature':>20s}  {'AUC w/o':>8s}  {'Delta':>8s}")

    for feat_i, feat_name in enumerate(FEATURE_NAMES):
        X_ablated = np.delete(X, feat_i, axis=1)
        fold_aucs = []
        for train_idx, val_idx in skf.split(X_ablated, y):
            m = LogisticRegression(max_iter=1000, C=best_c, solver="lbfgs", random_state=42)
            m.fit(X_ablated[train_idx], y[train_idx])
            prob = m.predict_proba(X_ablated[val_idx])[:, 1]
            fold_aucs.append(roc_auc_score(y[val_idx], prob))
        ablated_auc = np.mean(fold_aucs)
        delta = ablated_auc - baseline_auc
        print(f"  {feat_name:>20s}  {ablated_auc:.4f}    {delta:+.4f}")

    return best_c


def _compute_ece(y_true: np.ndarray, y_prob: np.ndarray, n_bins: int = 10) -> float:
    """Expected Calibration Error: weighted average of per-bin |accuracy - confidence|."""
    bin_edges = np.linspace(0, 1, n_bins + 1)
    ece = 0.0
    for lo, hi in zip(bin_edges[:-1], bin_edges[1:]):
        mask = (y_prob >= lo) & (y_prob < hi)
        if mask.sum() == 0:
            continue
        bin_acc = y_true[mask].mean()
        bin_conf = y_prob[mask].mean()
        ece += mask.sum() / len(y_true) * abs(bin_acc - bin_conf)
    return ece


def export_model(X: np.ndarray, y: np.ndarray, best_c: float):
    """Train final model on ALL data with best C, export to ONNX."""
    print("\n" + "=" * 50)
    print(f"Training final model (C={best_c}) on all {X.shape[0]} samples")
    print("=" * 50)

    model = LogisticRegression(
        max_iter=1000,
        C=best_c,
        solver="lbfgs",
        random_state=42,
    )
    model.fit(X, y)

    # Feature coefficients
    print("\nFeature coefficients:")
    for name, coef in zip(FEATURE_NAMES, model.coef_[0]):
        print(f"  {name:20s} {coef:+.6f}")
    print(f"  {'intercept':20s} {model.intercept_[0]:+.6f}")

    # Build ONNX model from scratch for exact C++ compatibility
    # (single input "X", single output "win_prob")
    from onnx import helper, TensorProto, numpy_helper

    W = model.coef_.astype(np.float32)        # shape [1, 7]
    b = model.intercept_.astype(np.float32)    # shape [1]

    W_init = numpy_helper.from_array(W.T, name="W")   # [7, 1] for MatMul
    b_init = numpy_helper.from_array(b, name="b")

    X_input = helper.make_tensor_value_info("X", TensorProto.FLOAT, [1, NUM_FEATURES])
    Y_output = helper.make_tensor_value_info("win_prob", TensorProto.FLOAT, [1, 1])

    matmul = helper.make_node("MatMul", ["X", "W"], ["matmul_out"])
    add = helper.make_node("Add", ["matmul_out", "b"], ["logit"])
    sigmoid = helper.make_node("Sigmoid", ["logit"], ["win_prob"])

    graph = helper.make_graph(
        [matmul, add, sigmoid],
        "win_prob_model",
        [X_input],
        [Y_output],
        initializer=[W_init, b_init],
    )

    onnx_model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    onnx_model.ir_version = 7
    onnx.checker.check_model(onnx_model)

    os.makedirs(os.path.dirname(MODEL_PATH), exist_ok=True)
    onnx.save(onnx_model, MODEL_PATH)
    model_size = os.path.getsize(MODEL_PATH)
    print(f"\nModel saved to {MODEL_PATH} ({model_size} bytes)")
    print(f"Input: X float32 [1, {NUM_FEATURES}]")
    print(f"Output: win_prob float32 [1, 1]")

    # Quick verification
    import onnxruntime as ort
    sess = ort.InferenceSession(MODEL_PATH)
    test_input = np.array([[5.0, 3.0, 600.0, 1.0, 0.0, 50.0, 0.6]], dtype=np.float32)
    result = sess.run(["win_prob"], {"X": test_input})
    print(f"\nVerification: input={test_input[0].tolist()}")
    print(f"  win_prob = {result[0][0][0]:.4f}")


def main():
    print("Cortex Win Probability Model Training")
    print("=" * 50)
    print(f"Database: {DB_CONN}")
    print(f"Features: {NUM_FEATURES}")

    conn = psycopg2.connect(DB_CONN)
    try:
        X, y = fetch_training_data(conn)
        best_c = evaluate_model(X, y)
        export_model(X, y, best_c)
    finally:
        conn.close()

    print("\nDone.")


if __name__ == "__main__":
    main()
