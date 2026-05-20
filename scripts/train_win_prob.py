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
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score, log_loss, roc_auc_score
import onnx
from skl2onnx import convert_sklearn
from skl2onnx.common.data_types import FloatTensorType

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


def train_and_export(X: np.ndarray, y: np.ndarray):
    """Train logistic regression and export to ONNX."""
    print(f"\nTraining data: {X.shape[0]} samples, {X.shape[1]} features")
    print(f"Home win rate: {y.mean():.3f}")

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )

    model = LogisticRegression(
        max_iter=1000,
        C=1.0,
        solver="lbfgs",
        random_state=42,
    )
    model.fit(X_train, y_train)

    # Evaluate
    y_pred = model.predict(X_test)
    y_prob = model.predict_proba(X_test)[:, 1]

    acc = accuracy_score(y_test, y_pred)
    auc = roc_auc_score(y_test, y_prob)
    ll = log_loss(y_test, y_prob)

    print(f"\nTest accuracy: {acc:.4f}")
    print(f"Test AUC:      {auc:.4f}")
    print(f"Test log loss: {ll:.4f}")

    # Feature importance (coefficients)
    feature_names = [
        "score_diff", "quarter", "sec_remaining",
        "home_advantage", "momentum", "elo_diff", "elo_expected"
    ]
    print("\nFeature coefficients:")
    for name, coef in zip(feature_names, model.coef_[0]):
        print(f"  {name:20s} {coef:+.6f}")
    print(f"  {'intercept':20s} {model.intercept_[0]:+.6f}")

    # Export to ONNX
    initial_type = [("X", FloatTensorType([None, NUM_FEATURES]))]
    onnx_model = convert_sklearn(
        model, initial_types=initial_type,
        target_opset=13,
        options={id(model): {"zipmap": False}},
    )

    # Rename output to "win_prob" for compatibility with C++ code
    # The sklearn converter creates "label" and "probabilities" outputs.
    # We need to add a post-processing step or just use the probabilities output.
    # For simplicity, we'll create a clean model manually.

    # Actually, let's build the ONNX model from scratch for exact compatibility
    # with the existing C++ code (single input "X", single output "win_prob")
    import onnx
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
        train_and_export(X, y)
    finally:
        conn.close()

    print("\nDone.")


if __name__ == "__main__":
    main()
