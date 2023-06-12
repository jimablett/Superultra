# Superultra (NNUE)

## Overview
Superultra is a UCI compliant chess engine that uses an alpha-beta search framework, various heuristics and optimization techniques, and an efficiently updatable neural network (NNUE) in order to analyze chess positions and calculate moves.

## Usage
Superultra supports the UCI Protocol but does not come with a GUI (like most UCI chess engines). It is recomended to download a UCI Compatable GUI such as cutechess.

## Building
```
cd src
make
```

## Features

### Board Representation
* Bitboards
* Magic Bitboards for efficient slider attack generation

### Search
* Iterative Deepening
* Aspiration Windows
* Parallel Search with Lazy SMP
* Principle Variation Search
* Transposition Table with 4 buckets and aging (shared across threads)
* Move Ordering
  * Countermove Heuristic
  * Killer Heuristic
  * Continuation History Heuristic and History Heuristic (with gravity) 
  * MVV/LVA
  * SEE
  * Transposition Table Move
* Pruning, Reductions, and Extensions
  * Null Move Pruning 
  * Razoring
  * Reverse Futility Pruning
  * Probcut
  * Quiet Move Pruning
    * Move Count Pruning
    * Futility Pruning
    * History Pruning
  * SEE Pruning
  * Singular Extensions + Multicut Pruning
  * Late Move Reductions
* Quiescence Search
* Time Management
  * Best move stability
  * Score stability
  * Complexity based on the percentage of time spent searching moves that are not the best move 

### Evaluation
* Efficiently Updatable Neural Network
* (768x6-->384)x2-->1 architecture
  *  Perspective
  *  Crelu Activation Function
  *  6 King Buckets

## More About the NNUE
The Neural Network is trained by a C++ trainer (insert link here later) that I wrote myself. It is important to note that the training data I used (2.3 billion positions) is generated by external engines due to a lack of hardware resources. Specifically, I used data from the <a href="https://lczero.org/blog/2021/04/jumping-on-the-nnue-bandwagon">lc-fish project</a>.

## Acknowledgements
<a href="https://www.chessprogramming.org/Main_Page">Chess Programming Wiki</a> was a great stepping stone. I learned more advanced techniques, tricks, and what works and what doesn't through looking at the following engines. 

* <a href="https://github.com/PGG106/Alexandria">Alexandria</a>
    * I am also using Alexandria's MakeFile
* <a href="https://github.com/jhonnold/berserk">Berserk</a>
* <a href="https://github.com/SzilBalazs/BlackCore">Black Core</a>
* <a href="https://github.com/AndyGrant/Ethereal">Ethereal</a>
* <a href="https://github.com/rafid-dev/rice">Rice</a>
* <a href="https://github.com/Disservin/Smallbrain">Small Brain</a>
* <a href="https://github.com/mhouppin/stash-bot">Stash</a>
* <a href="https://github.com/official-stockfish/Stockfish">Stockfish</a>
