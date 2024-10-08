#include "search.h"
#include "timecontrol.h"
#include "movescore.h"
#include "uci.h"
#include <math.h>
#include <thread>
#include <vector>
#include <cstring>
#include <algorithm>

static timeMan tm;
static uint64 nodeLim;
static std::vector<std::thread> threads;
static std::vector<searchData> threadSD;
static Depth lmrReduction[maximumPly + 5][maxMovesInTurn];

bool pondering = false;

void initLMR(){
    for (Depth depth = 1; depth <= maximumPly; depth++){
        for (int i = 0; i < maxMovesInTurn; i++){
            lmrReduction[depth][i] = 1.5 + log(depth) * log(i + 1) / 2;
        }
    }
}



void setThreadCount(int tds) {
    threadCount = tds;

    while (static_cast<int>(threads.size()) < tds) {
        threads.emplace_back();
        threadSD.emplace_back();
    }
    while (static_cast<int>(threads.size()) > tds) {
        threads.pop_back();
        threadSD.pop_back();
    }
}







void resetAllSearchDataNonHistory(){
    for (int td = 0; td < threadCount; td++){
        threadSD[td].resetNonHistory(td);
    }
}

void decayAllSearchDataHistory(){
    for (int td = 0; td < threadCount; td++){
        threadSD[td].decayHistory();
    }
}

void clearAllSearchDataHistory(){
    for (int td = 0; td < threadCount; td++){
        threadSD[td].clearHistory();
    }
}

void endSearch(){
    tm.forceStop = true;
}

static inline void checkEnd(searchData &sd){
    sd.stopped = tm.stopDuringSearch();

    // If we have a node limit and are at thread 0, check our node count
    if (nodeLim and sd.threadId == 0){
        uint64 nodeCount = 0;
        for (int i = 0; i < threadCount; i++){
            nodeCount += threadSD[i].nodes;
        }
        if (nodeCount >= nodeLim){
            sd.stopped = true;
        }
    }
}

static inline void adjustEval(ttEntry &tte, Score &staticEval){
    // Adjust evaluation based on TT by snapping our static evaluation to the
    // TT score based on bound. We assume that tte exists and has a score
    
    if (decodeBound(tte.ageAndBound) == boundExact
        or (decodeBound(tte.ageAndBound) == boundLower and staticEval < tte.score)
        or (decodeBound(tte.ageAndBound) == boundUpper and staticEval > tte.score))
    {
        staticEval = tte.score;
    }
}

template<bool pvNode> static Score qsearch(Score alpha, Score beta, Depth ply, position &board, searchData &sd, searchStack *ss){
    // Step 1) Leaf node and misc stuff
    // Update information and check if we should stop

    sd.selDepth = std::max(sd.selDepth, ply);
    
    if ((sd.nodes & 2047) == 0){
        checkEnd(sd);
    }
    if (sd.stopped){
        return 0;
    }
    if (board.drawByRepetition(ply) or board.drawByInsufficientMaterial() or board.drawByFiftyMoveRule()){
        return 1 - (sd.nodes & 2);
    }
    if (ply > maximumPly){
        return board.eval();
    }

    // Step 2) Probe the TT and initalize variables
    // Get TT values and other variables and check if we can end early

    ttEntry tte = ttEntry();
    bool foundEntry = globalTT.probe(board.getHash(), tte, ply);

    Score originalAlpha = alpha;
    bool inCheck = board.inCheck();

    ss->staticEval = noScore;

    if (foundEntry and !pvNode){
        if (decodeBound(tte.ageAndBound) == boundExact 
            or (decodeBound(tte.ageAndBound) == boundLower and tte.score >= beta)
            or (decodeBound(tte.ageAndBound) == boundUpper and tte.score <= alpha))
        {
            return tte.score;
        }
    }

    // Step 3) Static evaluation
    // Query the NNUE if we haven't gotten a static eval from the TT and adjust the 
    // static eval based on TT. Also do the qsearch standing pat heuristic

    if (!inCheck){
        ss->staticEval = foundEntry ? tte.staticEval : board.eval();

        // Adjust static eval based on TT (~10 elo)
        // Don't do it if we have mate score and are at PV or else we will return improper mating PV list
        // and things get messy since bestScore may not be a mate score

        if (foundEntry and tte.score != noScore and abs(tte.score) < foundMate){
            adjustEval(tte, ss->staticEval);
        }
        
        // Assume that there is some quiet/capture that will allow the static eval to remain above
        if (ss->staticEval >= beta){
            return ss->staticEval;
        }

        alpha = std::max(alpha, ss->staticEval);
    }

    // Step 4) Move gen and related 
    // Note that we don't consider draws in qsearch and we generate ALL MOVES when in check. This means
    // we return mate score if it's mate. Also, we don't have to worry about improper PV list since a
    // mate score will never be propagated to the root of the QS due to maxing alpha with static eval

    moveList moves;
    board.genAllMoves(!inCheck, moves);
    
    if (moves.sz == 0){
        return inCheck ? -(checkMateScore - ply) : ss->staticEval;
    }
    
    scoreMoves(moves, foundEntry ? tte.bestMove : nullOrNoMove, ply, board, sd, ss);

    // Step 5) Iterate over moves
    // Note that we set bestScore to -checkMateScore but we will max it with standingPat after the loop

    Score bestScore = -checkMateScore;
    Move bestMove = nullOrNoMove;

    for (int i = 0; i < moves.sz; i++){
        // Step 6) Bring best move forwards and initialize
        // Pretty self explanatory...

        moves.bringBest(i);
        Move move = moves.moves[i].move;
        Movescore mscore = moves.moves[i].score;

        // Step 7) SEE Pruning (~6.5 elo)
        // Skip moves with bad SEE. First if statement skips all moves the moment
        // we hit a losing move determined in move ordering. The second statement
        // ignores all moves with a losing SEE including quiet moves 

        if (bestScore > -foundMate and mscore < okayThresholdScore){
            break;
        }
        if (bestScore > -foundMate and !board.seeGreater(move, -50)){
            continue;
        }

        // Step 8) Make and update
        // Update appropriate history indices, make move, and prefetch TT

        ss->move = move;
        ss->counter = &(sd.counter[board.movePieceEnc(move)][moveTo(move)]);
        ss->contHist = &(sd.contHist[board.moveCaptType(move) != noPiece][board.movePieceEnc(move)][moveTo(move)]);
        sd.nodes++;

        board.makeMove(move);
        globalTT.prefetch(board.getHash());

        // Step 9) Recurse
        // Simple stuff, no zero window

        Score score = -qsearch<pvNode>(-beta, -alpha, ply + 1, board, sd, ss + 1);

        // Step 10) Undo and update
        // Undo and see if we raise alpha or have a beta cutoff

        board.undoLastMove();

        if (score > bestScore){
            bestScore = score;
            bestMove = move;
            
            // Raise alpha
            if (score > alpha){
                alpha = score;

                // Beta cutoff 
                if (score >= beta){
                    break;
                }
            }
        }
    }

    // Step 11) TT Stuff
    // Update bestScore with static eval and put result in TT

    // We max bestScore with static eval late so that we will always have a best move
    if (!inCheck){
        bestScore = std::max(bestScore, ss->staticEval);
    }

    // Update TT
    if (!sd.stopped){
        TTboundAge bound = boundExact;

        if (bestScore <= originalAlpha){
            bound = boundUpper;
        }
        else if (bestScore >= beta){
            bound = boundLower;
        }
        globalTT.addToTT(board.getHash(), bestScore, ss->staticEval, bestMove, 0, ply,bound, pvNode);
    }
    return bestScore;
}

template<bool pvNode, bool cutNode> static Score negamax(Score alpha, Score beta, Depth ply, Depth depth, position &board, searchData &sd, searchStack *ss){
    // 1) Leaf node and misc stuff
    // Update information and check if we should stop

    sd.pvLength[ply] = ply;
    sd.selDepth = std::max(sd.selDepth, ply);
    
    if ((sd.nodes & 2047) == 0){
        checkEnd(sd);
    }
    if (sd.stopped){
        return 0;
    }
    if (ply > maximumPly){
        return board.eval();
    }
    if (depth <= 0){
        return qsearch<pvNode>(alpha, beta, ply, board, sd, ss);
    }
    if (board.drawByRepetition(ply) or board.drawByInsufficientMaterial() or board.drawByFiftyMoveRule()){
        return 1 - (sd.nodes & 2);
    }
    
    // Step 2) Mate distance pruning (~0.5 elo but good for finding mates)
    // We prunes trees that have no hope of improving our mate score (if we have one).
    // Upperbound of score for our current node: checkMateScore - ply - 1
    // Lowerbound of score for our current node: -checkMateScore + ply

    if (ply > 0){
        alpha = std::max(alpha, static_cast<Score>(-checkMateScore + ply));
        beta = std::min(beta, static_cast<Score>(checkMateScore - ply - 1));

        if (alpha >= beta){
            return alpha;
        }
    }

    // 3) Probe the TT and initialize variables
    // Get TT values and other variables and check if we can end early

    ttEntry tte = ttEntry();
    bool foundEntry = ss->excludedMove == nullOrNoMove ? globalTT.probe(board.getHash(), tte, ply) : false;

    Score originalAlpha = alpha;
    bool inCheck = board.inCheck();

    ss->staticEval = noScore;

    if (foundEntry
        and !pvNode
        and tte.depth >= depth)
    {
        if (decodeBound(tte.ageAndBound) == boundExact 
            or (decodeBound(tte.ageAndBound) == boundLower and tte.score >= beta)
            or (decodeBound(tte.ageAndBound) == boundUpper and tte.score <= alpha))
        {
            return tte.score;
        }
    }

    // Step 4) TT move reduction (aka internal "iterative" reduction) (~5 elo)
    // If we don't have a TT move, we reduce by 1

    if ((pvNode or cutNode)
        and depth >= 4 
        and !foundEntry 
        and ss->excludedMove == nullOrNoMove)
    {
        depth--;
    }

    // Step 5) Static eval and improving
    // Get the static evaluation if not in TT and see if we are improving by comparing static eval with
    // that of 2 plies ago. Note that adjusting eval here based on TT like what we did in qsearch
    // loses elo for some unknown reason...

    if (!inCheck){
        ss->staticEval = foundEntry ? tte.staticEval : board.eval();
    }
    bool improving = (!inCheck and (ply >= 2 and ((ss - 2)->staticEval == noScore or ss->staticEval > (ss - 2)->staticEval)));

    // Step 6) Reverse Futility Pruning (~75 elo)
    // If the static evaluation is far above beta, we can assume that it will fail high

    if (!pvNode 
        and !inCheck
        and ss->excludedMove == nullOrNoMove
        and depth <= 8
        and ss->staticEval - 77 * std::max(depth - improving, 0) >= beta)
    {
        return ss->staticEval;
    }

    // Step 7) TT based razoring (~4 elo)
    // At nodes near the leaf, we can see if the tt score puts us far below alpha. If so
    // we assume that it will be impossible to raise it to alpha.
    
    if (!pvNode 
        and !inCheck 
        and ss->excludedMove == nullOrNoMove
        and depth <= 2
        and foundEntry 
        and (decodeBound(tte.ageAndBound) == boundUpper or decodeBound(tte.ageAndBound) == boundExact) 
        and tte.score + 180 * depth * depth <= alpha)
    {
        return tte.score;
    }

    // Step 8) Null Move Pruning (~60 elo)
    // We evaluate the position if we skipped our turn and check if doing so
    // causes a beta cutoff via the zero window [beta - 1, beta]

    if (!pvNode 
        and !inCheck
        and ss->excludedMove == nullOrNoMove
        and depth >= 3 
        and ss->staticEval >= beta
        and (ply >= 1 and (ss - 1)->move != nullOrNoMove)
        and board.hasMajorPieceLeft(board.getTurn()))
    {
        // Make move and update variables
        ss->move = nullOrNoMove;
        sd.nodes++;

        board.makeNullMove();
        globalTT.prefetch(board.getHash());
        
        
        Depth R = 3 + (depth / 3) + std::min(static_cast<int>(ss->staticEval - beta) / 200, 3);
        
        // Search
        Score score = -negamax<false, !cutNode>(-beta, -(beta - 1), ply + 1, depth - R, board, sd, ss + 1);

        // Undo
        board.undoNullMove();

        // See if score is above beta
        if (score >= beta){
            // Don't use mate score
            if (score >= foundMate){
                score = beta;
            }
            return score;
        }
    }
    
    // Step 9) Probcut (~11.5 elo)
    // probCutBeta is a calculated value above beta. We try promising tactical moves and if any
    // of them have a score higher than probCutBeta at a reduced depth then we can assume that move will 
    // will have a score higher than beta at a normal depth
    
    Score probCutBeta = std::min(static_cast<int>(beta + 200 - 40 * improving), static_cast<int>(checkMateScore));
    
    if (!pvNode 
        and !inCheck
        and ss->excludedMove == nullOrNoMove
        and depth >= 5
        and abs(beta) < foundMate 
        and !(foundEntry and tte.score < probCutBeta and tte.depth + 3 >= depth))
    {
        // We only try noisy moves
        moveList probCutMoves;
        board.genAllMoves(true, probCutMoves);
        
        scoreMoves(probCutMoves, foundEntry ? tte.bestMove : nullOrNoMove, ply, board, sd, ss);

        for (int i = 0; i < probCutMoves.sz; i++){
            probCutMoves.bringBest(i);
            Move move = probCutMoves.moves[i].move;

            // SEE that will put us above probCutBeta
            if (!board.seeGreater(move, probCutBeta - ss->staticEval)){
                continue;
            }

            // Perform the move and make relevant updates
            ss->move = move;
            ss->counter = &(sd.counter[board.movePieceEnc(move)][moveTo(move)]);
            ss->contHist = &(sd.contHist[board.moveCaptType(move) != noPiece][board.movePieceEnc(move)][moveTo(move)]);
            sd.nodes++;

            board.makeMove(move);
            globalTT.prefetch(board.getHash());

            // Verify with QS
            Score score = -qsearch<false>(-probCutBeta, -(probCutBeta - 1), ply + 1, board, sd, ss + 1);
            
            // If verified, normal search with reduced depth
            if (score >= probCutBeta){
                score = -negamax<false, !cutNode>(-probCutBeta, -(probCutBeta - 1), ply + 1, depth - 4, board, sd, ss + 1);
            }
            
            // Undo last move
            board.undoLastMove();

            // Prune as this move will likely fail high when searched with a normal depth
            if (score >= probCutBeta){
                // Store entry in TT
                globalTT.addToTT(board.getHash(), score, ss->staticEval, move, depth - 3, ply, boundLower, pvNode);
                return score;
            }
        }
    }
    
    // Step 10) Generate moves, end of game checking, and move scoring
    // Generate moves and if we have 0 moves then the game ended. Also score the moves...

    moveList moves, quiets;
    board.genAllMoves(false, moves);
     
    if (moves.sz == 0){
        return inCheck ? -(checkMateScore - ply) : 0;
    }
    scoreMoves(moves, foundEntry ? tte.bestMove : nullOrNoMove, ply, board, sd, ss);

    // Step 11) Iterate over the moves
    // Pretty self explanatory...

    Score bestScore = -checkMateScore;
    Move bestMove = nullOrNoMove;
    int movesSeen = 0;

    for (int i = 0; i < moves.sz; i++){
        // Step 12) Variable stuff
        // Bring best move forwards and declare necessary variables and do updates

        moves.bringBest(i);
        Move move = moves.moves[i].move;

        if (ss->excludedMove != nullOrNoMove and move == ss->excludedMove){
            continue;
        }

        Score score = checkMateScore;
        Movescore history = 0;
        Depth extension = 0;
        uint64 nodesBefore = sd.nodes;

        bool ttSoundCapt = (foundEntry and tte.depth > 0 and board.moveCaptType(tte.bestMove) != noPiece);
        bool isQuiet = movePromo(move) == noPiece and board.moveCaptType(move) == noPiece;
        bool killerOrCounter = (move == sd.killers[ply][0] or move == sd.killers[ply][1] or (ply >= 1 and (ss - 1)->move != nullOrNoMove and move == *((ss - 1)->counter)));

        movesSeen++;

        if (isQuiet){
            quiets.addMove(move);
            history = getQuietHistory(move, ply, board, sd, ss);
        }
        
        // Step 13) Quiet move pruning (~70 elo)
        // We skip quiet moves for various reasons. Note that we use lmrDepth for futility pruning and history based
        // pruning because we want the lateness in move ordering to affect pruning. However, there is no need to use
        // lmrDepth in move count pruning since we are literally pruning nodes if they are late...

        if (isQuiet and bestScore > -foundMate and !inCheck){
            Depth lmrDepth = std::max(1, depth - lmrReduction[depth][i]);

            // A) Quiet Move Count Pruning (~14 elo)
            // If we are at low depth and searched enough quiet moves we can stop searching all other quiet moves

            if (depth <= 4
                and quiets.sz >= 1 + 3 * depth * depth + improving)
            {
                continue;
            }

            // B) Futility Pruning (~20 elo)
            // Skip quiet moves if we are way below alpha

            if (lmrDepth <= 4
                and ss->staticEval + 110 + 75 * lmrDepth + history / 160 < alpha)
            {
                continue;
            }

            // C) History Based Pruning (~9 elo)
            // Prune non-killer and non-counter quiet moves that have a bad history
            
            if (!killerOrCounter
                and lmrDepth <= 2
                and history <= -1408 * depth - 256 * improving)
            {
                continue;
            }
        }
        
        // Step 14) SEE Pruning (~25 elo)
        // We skip moves with a bad SEE. More elo can be gained by treating quiet and noisy
        // moves differently

        if (bestScore > -foundMate 
            and depth <= 5
            and movesSeen >= 3)
        {
            Score cutoff = isQuiet ? -60 * depth : -55 * depth;

            if (!board.seeGreater(move, cutoff)){
                continue;
            }
        }

        // Step 15) Singular Extension and Multi-cut Pruning (~38 elo from SE + MCP, ~4 elo from DE, ~7 elo from Alpha NE)
        // If our TT move singularly performs better than all other moves at a reduced depth then we should extend
        // it. However, if a bunch of moves manage to fail high at a reduced depth then we can assume
        // that at least one of them will fail high at a normal depth

        if (ply > 0
            and depth >= 7
            and foundEntry 
            and move == tte.bestMove
            and tte.depth >= depth - 3 
            and (decodeBound(tte.ageAndBound) == boundLower or decodeBound(tte.ageAndBound) == boundExact)
            and abs(tte.score) < foundMate)
        {
            // Check if any other moves at a depth of singularDepth is kinda as good as our tt move's score
            Score singularBeta = tte.score - 3 * depth;
            Depth singularDepth = (tte.depth - 1) / 2;
            
            // Search all other moves -- call search on the same position but exclude the TT move
            ss->excludedMove = move;
            Score singularScore = negamax<false, cutNode>(singularBeta - 1, singularBeta, ply, singularDepth, board, sd, ss);
            ss->excludedMove = nullOrNoMove;

            // Our TT move is singular meaning it's better than all other moves by some margin
            if (singularScore < singularBeta){
                extension = 1;

                // Double extend if singular score is way worse than singular beta (meaning TT is way better than everyone else)
                if (!pvNode
                    and singularScore < singularBeta - 25
                    and (ss - 1)->dextension <= 4)
                {
                    extension = 2;
                }
            }

            // Multicut -- our TT move and at least one other move fails high (at a reduced depth)
            else if (singularBeta >= beta){
                return singularBeta;
            }
            
            // Probable Multicut (since TT move is greater/equal to beta and some other move is greater/equal to singularBeta)
            else if (tte.score >= beta){
                extension = -1 - !pvNode;
            }

            // If we predict we are gonna fail high and another move has score either slightly less or greater 
            // than TT score then we are in a situation similar to probable multicut
             
            else if (cutNode){
                extension = -1;
            }
            
            // Both our TT score and the "second" best score (at a reduced depth) fail to raise alpha
            else if (tte.score <= alpha and singularScore <= alpha){
                extension = -1;
            }
        }

        // Step 16) Make and update
        // Update necessary info, make move, and prefetch
        
        ss->move = move;
        ss->counter = &(sd.counter[board.movePieceEnc(move)][moveTo(move)]);
        ss->contHist = &(sd.contHist[board.moveCaptType(move) != noPiece][board.movePieceEnc(move)][moveTo(move)]);
        ss->dextension = (ss - 1)->dextension + (extension > 1);
        sd.nodes++;

        board.makeMove(move);
        globalTT.prefetch(board.getHash());

        // Step 17) Late Move Reduction (~175 elo)
        // Later moves are likely to fail low so we search them at a reduced depth
        // with zero window [alpha, alpha + 1]. We do a re-search if it does not fail low

        if (!inCheck
            and depth >= 3
            and movesSeen >= 3 + 2 * pvNode)
        {
            Depth R = lmrReduction[depth][i];
            
            // Decrease reduction if we are currently at a PV node
            R -= pvNode;

            // Decrease reduction if non-quiet
            R -= !isQuiet;

            // Decrease reduction if special quiet
            R -= killerOrCounter;

            // Increase reduction if our TT move is a capture
            R += ttSoundCapt;
            
            // Increase reduction if we aren't improving
            R += !improving;

            // If quiet, adjust reduction based on history
            if (isQuiet){
                R -= std::clamp(static_cast<int>(history) / 4096, -2, 2);
            }

            // Don't drop into qsearch
            R = std::min(R, static_cast<Depth>(depth + extension - 1));

            // Reduce as long as there is some reduction
            if (R >= 2){
                score = -negamax<false, !cutNode>(-(alpha + 1), -alpha, ply + 1, depth + extension - R, board, sd, ss + 1);
            }
        }

        // Step 18) Principle Variation Search
        // We do this if LMR inconclusive or we didn't do LMR (note that score is initializd to inf)

        if (score > alpha){
            // Fully evaluate node if first move
            if (movesSeen == 1){
                score = -negamax<true, false>(-beta, -alpha, ply + 1, depth + extension - 1, board, sd, ss + 1);
            }
            
            // Otherwise do zero window search: [alpha, alpha + 1]
            else{ 
                score = -negamax<false, !cutNode>(-(alpha + 1), -alpha, ply + 1, depth + extension - 1, board, sd, ss + 1); 
                        
                // Zero window inconclusive (note that its only possible to enter this if pvNode)
                if (score > alpha and score < beta){
                    score = -negamax<true, false>(-beta, -alpha, ply + 1, depth + extension - 1, board, sd, ss + 1);
                }
            }
        }

        // Step 19) Unmake and update
        // Undo and see if we raise alpha or have a beta cutoff and update necessary info

        board.undoLastMove();

        // If at root, update amount of time we spent searching that move
        if (ply == 0){
            sd.moveNodeStat[moveFrom(move)][moveTo(move)] += sd.nodes - nodesBefore;
        }

        if (score > bestScore){
            bestScore = score;
            bestMove = move;

            if (score > alpha){
                alpha = score;

                // Update PV table (update before we test for beta cutoff since we may have beta
                // cutoffs at PV nodes due to snapping beta in mate distance pruning or EGTB and
                // we need to log the moves that caused the beta cutoff)

                if (pvNode){
                    sd.pvTable[ply][ply] = move;           
                    sd.pvLength[ply] = sd.pvLength[ply + 1];

                    for (Depth nextPly = ply + 1; nextPly < sd.pvLength[ply]; nextPly++){
                        sd.pvTable[ply][nextPly] = sd.pvTable[ply + 1][nextPly];
                    }
                }
                
                // Beta cutoff
                if (score >= beta){
                    if (isQuiet){
                        updateAllHistory(move, quiets, depth, ply, board, sd, ss);
                    }
                    break;
                }
            }
        }
    }

    // Step 20) Update TT
    // Put search results into TT 

    if (!sd.stopped and ss->excludedMove == nullOrNoMove){
        TTboundAge bound = boundExact;

        if (bestScore <= originalAlpha){
            bound = boundUpper;
        }
        else if (bestScore >= beta){
            bound = boundLower;
        }
        globalTT.addToTT(board.getHash(), bestScore, ss->staticEval, bestMove, depth, ply, bound, pvNode);
    }
    return bestScore;
}

Score aspirationWindowSearch(Score prevEval, Depth depth, position &board, searchData &sd){
    // Aspiration Window (~35 elo)
    // We assume our current evaluation will be very close
    // to our previous evaluation so we search with a small window around
    // our previous evaluation and expand the window if it is inconclusive

    // Init window and stack
    int delta = 14;
    searchStack searchArr[maximumPly + 5];
    searchStack *ss = searchArr + 2;
    
    for (int i = 0; i < maximumPly + 5; i++){
        searchArr[i].excludedMove = nullOrNoMove;
        searchArr[i].dextension = 0;
    }

    // Now we init alpha and beta with our window
    Score alpha = std::max(static_cast<int>(prevEval - delta), static_cast<int>(-checkMateScore));
    Score beta = std::min(static_cast<int>(prevEval + delta), static_cast<int>(checkMateScore));

    // If we are at low depth, we just do a single search since low depths are unstable
    if (depth <= 7){
        alpha = -checkMateScore;
        beta = checkMateScore;
    }    

    while (true){
        // Get score
        Score score = negamax<true, false>(alpha, beta, 0, depth, board, sd, ss);

        // Out of time
        if (sd.stopped){
            break;
        }
        
        // Score is an upperbound
        if (score <= alpha){
            beta = (static_cast<int>(alpha) + static_cast<int>(beta)) / 2;
            alpha = std::max(static_cast<int>(score - delta), static_cast<int>(-checkMateScore));
        }

        // Score is a lowerbound
        else if (score >= beta){
            // No alpha = (alpha + beta) / 2 because "it doesn't work" according to discord person (they were correct)
            beta = std::min(static_cast<int>(score + delta), static_cast<int>(checkMateScore));


            // Reduction
            if (abs(score) < foundMate and depth >= 8){
                depth--;
            }
        }

        // Score is exact
        else{
            return score;
        }
        
        // Snap to mate score
        if (alpha <= 2750){
            alpha = -checkMateScore;
        }
        if (beta >= 2750){
            beta = checkMateScore;
        }

        // Increase delta exponentially
        delta += delta / 2;
    }
    // Only reach here when out of time
    return 0;
}

void printSearchResults(searchResultData result){
    // First get "background info"
    uint64 totalNodes = 0;
    TimePoint timeSpent = tm.timeSpent();
    uint64 hashFull = globalTT.hashFullness();

    for (int i = 0; i < threadCount; i++){
        totalNodes += threadSD[i].nodes;
    }

    // Now print out all info
    std::cout<<"info depth "<<int(result.depthSearched);
    std::cout<<" seldepth "<<int(result.selDepth);
    
    if (abs(result.score) >= foundMate){
        std::cout<<" score mate "<<(checkMateScore - abs(result.score) + 1) * (result.score > 0 ? 1 : -1) / 2;
    }
    else{
        std::cout<<" score cp "<<result.score;
    }
            
    std::cout<<" nodes "<<totalNodes;
    std::cout<<" time "<<timeSpent;
    std::cout<<" nps "<<int(totalNodes / (timeSpent / 1000.0 + 0.00001));
    std::cout<<" hashfull "<<hashFull;
    std::cout<<" pv ";

    for (std::string mv : result.pvMoves){
        std::cout<<mv<<" ";
    }
    std::cout<<std::endl;
}

void selectBestThread(){
    // We use thread 0 to report info and keep track of time. However, it may not be the best
    // thread so we should select the best thread, report its info, and report its best move

    searchResultData bestResult = threadSD[0].result;

    for (int i = 1; i < threadCount; i++){
        searchResultData otherResult = threadSD[i].result;

        // If both are mating scores, use which one is closer
        if (abs(bestResult.score) >= foundMate and abs(otherResult.score) >= foundMate){
            if (abs(otherResult.score) > abs(bestResult.score)){
                bestResult = otherResult;
            }
        }

        // If both have equal depth, use whichever one has the higher score
        else if (bestResult.depthSearched == otherResult.depthSearched){
            if (otherResult.score > bestResult.score){
                bestResult = otherResult;
            }
        }

        // Otherwise, use the one with the higher depth
        else{
            if (otherResult.depthSearched > bestResult.depthSearched){
                bestResult = otherResult;
            }
        }
    }
    // Print the final result of the search
    printSearchResults(bestResult);
    std::cout<<"bestmove "<<bestResult.pvMoves[0];

    if (bestResult.pvMoves.size() >= 2){
        std::cout<<" ponder "<<bestResult.pvMoves[1];
    }
    std::cout<<std::endl;
}

void iterativeDeepening(position board, searchData &sd, Depth depthLim) {
    Score score = noScore;

    for (Depth startingDepth = 1; startingDepth <= depthLim; startingDepth++) {
        sd.selDepth = 0;
        score = aspirationWindowSearch(score, startingDepth, board, sd);

        if (!sd.stopped) {
            sd.result.depthSearched = startingDepth;
            sd.result.selDepth = sd.selDepth;
            sd.result.score = score;
            sd.result.pvMoves.clear();

            for (Depth i = 0; i < sd.pvLength[0]; i++) {
                sd.result.pvMoves.push_back(moveToString(sd.pvTable[0][i]));
            }

            if (sd.threadId == 0) {
                printSearchResults(sd.result);

                Move bestMove = sd.pvTable[0][0];
                tm.update(startingDepth, bestMove, score, 1.0 - (static_cast<double>(sd.moveNodeStat[moveFrom(bestMove)][moveTo(bestMove)]) / (sd.nodes + 1.0)));

                if (tm.stopAfterSearch()) {
                    break;
                }
            }
        } else {
            break;
        }
    }
}



void beginSearch(position board, uciSearchLims lims){
    // Deal with node and depth limits (if no depth limit, force it to be maximumPly)
    // Remember that 0 means the limit has not been set

    nodeLim = lims.nodeLim;

    if (!lims.depthLim)
        lims.depthLim = maximumPly;

    // Init
    tm.init(board.getTurn(), lims);
    resetAllSearchDataNonHistory();

    // Launch threadCount - 1 helper threads (start our indexing from 1)
    for (int i = 1; i < threadCount; i++){
        threads[i] = std::thread(iterativeDeepening, board, std::ref(threadSD[i]), lims.depthLim);
    }

    // Launch main thread
    iterativeDeepening(board, threadSD[0], lims.depthLim);
    
    // Once our main thread is done, stop and join helper threads
    endSearch();

    for (int i = 1; i < threadCount; i++){
        threads[i].join();
    }
    
    // Wait until we have officially been asked to stop pondering
    while (pondering);

    // Report and update
    selectBestThread();
    globalTT.incrementAge();
    decayAllSearchDataHistory();
}
