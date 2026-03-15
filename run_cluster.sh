#!/bin/bash

SESSION="dpu_relay_test"

tmux kill-session -t $SESSION 2>/dev/null
tmux new-session -d -s $SESSION

# connect to the cluster
tmux send-keys -t $SESSION "ssh dpu38" C-m
tmux split-window -h -t $SESSION
tmux send-keys -t $SESSION "ssh dpu37" C-m
tmux select-pane -t 0
tmux split-window -v -t $SESSION
tmux send-keys -t $SESSION "ssh c38" C-m
tmux select-pane -t 2
tmux split-window -v -t $SESSION
tmux send-keys -t $SESSION "ssh c37" C-m

tmux attach-session -t $SESSION