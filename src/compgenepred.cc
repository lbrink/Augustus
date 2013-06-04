/**********************************************************************
 * file: compgenepred.cc licence: Artistic Licence, see file
 * LICENCE.TXT or
 * http://www.opensource.org/licenses/artistic-license.php descr.:
 * comparative gene prediction on multiple species authors: Mario
 * Stanke, Alexander Gebauer, Stefanie König
 *
 * date    |   author      |  changes
 * --------|---------------|------------------------------------------
 * 07.03.12| Mario Stanke  | creation of the file
 * 06.10.12|Stefanie König | construction of an OrthoGraph from a set of
 *         |               | orthologous sequences + integration of
 *         |               | the optimization method
 **********************************************************************/

#include "compgenepred.hh"
#include "orthograph.hh"
#include "mea.hh"
#include "genomicMSA.hh"
#include "geneMSA.hh"
#include "orthoexon.hh"
#include "namgene.hh"

#include "contTimeMC.hh"
#include <gsl/gsl_matrix.h>
#include <ctime>

CompGenePred::CompGenePred(){
    if (Constant::Constant::dbaccess.empty()) { // give priority to database in case both exist
        rsa = new MemSeqAccess();
    } else {
        rsa = new DbSeqAccess();
    }
}

void CompGenePred::start(){

    // read in alignment, determine orthologous sequence fragments
    
#ifdef DEBUG
    cout << "reading in the phylogenetic tree" << endl;
#endif
    PhyloTree tree(Constant::treefile);  // has to be initialized before OrthoGraph
    ExonEvo evo;
    vector<double> branchset;
    tree.getBranchLengths(branchset);
    evo.setBranchLengths(branchset);
    evo.computeLogPmatrices();
    OrthoGraph::tree = &tree;
    GeneMSA::setTree(&tree);
    OrthoGraph::numSpecies = OrthoGraph::tree->numSpecies();

#ifdef DEBUG
    OrthoGraph::tree->printWithGraphviz("tree.dot");
    cout << "-------------------------------\nparameters phylogenetic model\n-------------------------------" << endl;
    cout << "rate exon loss:\t" << evo.getMu() << endl;
    cout << "rate exon gain:\t" << evo.getLambda() << endl;
    cout << "phylo factor:\t" << evo.getPhyloFactor() <<  "\n-------------------------------" << endl;
#endif

    bool dualdecomp;
    try {
	dualdecomp = Properties::getBoolProperty("/CompPred/dualdecomp");
    } catch (...) {
	dualdecomp = false;
    }

    //initialize output files of initial gene prediction and optimized gene prediction
    vector<ofstream*> baseGenes = initOutputFiles(".base"); // equivalent to MEA prediction
    vector<int> base_geneid(OrthoGraph::numSpecies, 1); // gene numbering
    vector<ofstream*> initGenes = initOutputFiles(".init"); // score added to all orthologous exons and penalty added to all non orthologous exons, then global path search repeated
    vector<int> init_geneid(OrthoGraph::numSpecies, 1);
    vector<ofstream*> optGenes = initOutputFiles();  //optimized gene prediction by applying majority rule move
    vector<int> opt_geneid(OrthoGraph::numSpecies, 1);
    vector<ofstream*> sampledExons = initOutputFiles(".sampled_ECs");

    BaseCount::init();
    PP::initConstants();
    NAMGene namgene; // creates and initializes the states
    FeatureCollection extrinsicFeatures; // hints, empty for now, will later read in hints for sequence ranges from database
    SequenceFeatureCollection sfc(&extrinsicFeatures);
    StateModel::readAllParameters(); // read in the parameter files: species_{igenic,exon,intron,utr}_probs.pbl

    // temporary tests of codon rate matrix stuff (Mario)
    double *pi = ExonModel::getCodonUsage();
    CodonEvo codonevo;
    codonevo.setKappa(2);
    codonevo.setPi(pi);
    vector<double> b; // all branch lengths occuring in the tree
    b.push_back(.5); // 50% codon substitutions between D.mel and D.pseudoo., 40% between human and mouse
    codonevo.setBranchLengths(b, 10);
    // codonevo.printBranchLengths();
    
    codonevo.setOmegas(40);
    cout << "Omegas, for which substitution matrices are stored:" << endl;
    codonevo.printOmegas();
    codonevo.computeLogPmatrices();
    
    // gsl_matrix *P = codonevo.getSubMatrixLogP(0.3, 0.25);
    // printCodonMatrix(P);
    GeneMSA::setCodonEvo(&codonevo);
  
    vector<string> speciesNames;
    OrthoGraph::tree->getSpeciesNames(speciesNames);
    GenomicMSA msa;
    msa.readAlignment(speciesNames);  // reads the alignment
    msa.prepareExons(); // merges alignment blocks if possible. Mario: TODO sort aligments in between
    vector<int> offsets;
    bool AlexFail;
    
    // determine object that holds a sequence range for each species
    // loop over species
    GeneMSA::openOutputFiles();
    while (GeneMSA *geneRange = msa.getNextGene()) {
        OrthoGraph orthograph;
	AlexFail = false; // temporary fix until Alexanders Bug is corrected
        for (int s = 0; s < speciesNames.size(); s++) {
            string seqID = geneRange->getSeqID(s);
            if (!seqID.empty()) {
		int start = geneRange->getStart(s);
		int end = geneRange->getEnd(s);
		// Steffi: gene Range must not exceed max length of 1000000 bps, otherwise sequence gets splits in doViterbiPiecewise!!!
		if(end-start+1 > 1000000){
		    throw ProjectError("compgenepred: sequence " + speciesNames[s] + "." + seqID + ":" + itoa(start) + ".." + itoa(end) + " exceeds 1000000bp");
		}
                AnnoSequence *seqRange = rsa->getSeq(speciesNames[s], seqID, start, end, geneRange->getStrand(s));
#ifdef DEBUG
                cout << "retrieving sequence:\t" << speciesNames[s] << ":" << seqID << "\t" << start << "-" << end << "\t";

                if( geneRange->getStrand(s) == plusstrand )
                    cout << "+\t";
                else
                    cout << "-\t";
                cout << "(" <<end - start + 1 << "bp)" << endl;
#endif
                if (seqRange==NULL) {
                    cerr << "random sequence access failed on " << speciesNames[s] << ", " << seqID << ", " << start << ", " << end << ", " << endl;
		    AlexFail = true;
                    break;
                } else {
                    namgene.getPrepareModels(seqRange->sequence, seqRange->length); // is needed for IntronModel::dssProb in GenomicMSA::createExonCands

                    if (geneRange->getStrand(s)==plusstrand) {
                        offsets.push_back(start);
                    } else {
                        offsets.push_back(geneRange->getSeqIDLength(s) - end - 1);
                    }
                    geneRange->createExonCands(seqRange->sequence); // identifies exon candidates on the sequence
                    list<ExonCandidate*> additionalExons = *(geneRange->getExonCands(s));

                    namgene.doViterbiPiecewise(sfc, seqRange, bothstrands); // sampling
                    list<Gene> *alltranscripts = namgene.getAllTranscripts();
                    if (alltranscripts){
                        cout << "building Graph for " << speciesNames[s] << endl;
                        /* build datastructure for graph representation
                         * @stlist : list of all sampled states
                         */
                        list<Status> stlist;
                        if(!alltranscripts->empty()){
                            buildStatusList(alltranscripts, false, stlist);
                        }
                        //build graph
                        orthograph.graphs[s] = new SpeciesGraph(&stlist, seqRange, additionalExons, speciesNames[s], geneRange->getStrand(s), sampledExons[s]);
                        orthograph.graphs[s]->buildGraph();

                        orthograph.ptrs_to_alltranscripts[s] = alltranscripts; //save pointers to transcripts and delete them after gene list is build
                    }
                }
            } else {
                offsets.push_back(0);
                geneRange->exoncands.push_back(NULL);
                geneRange->existingCandidates.push_back(NULL);
                cout<< speciesNames[s] << " doesn't exist in this part of the alignment."<< endl;
            }
        }
	if (!AlexFail){
	  geneRange->printGeneRanges();
	  geneRange->printExonCands(offsets);
	  geneRange->createOrthoExons(offsets, orthograph);
	  geneRange->printOrthoExons(rsa, offsets);
	  orthograph.all_orthoex = geneRange->getOrthoExons();
	  
	  orthograph.outputGenes(baseGenes,base_geneid);
	  //add score for selective pressure of orthoexons
	  orthograph.addScoreSelectivePressure();
	  //determine initial path
	  orthograph.globalPathSearch();
	  orthograph.outputGenes(initGenes,init_geneid);
	  
	  if(!orthograph.all_orthoex.empty()){
	      if(dualdecomp){ // optimization via dual decomposition
		  orthograph.dualdecomp(evo,100);
	      }
	      else{ // optimization by making small changes (moves)
		  orthograph.pruningAlgor(evo);
		  orthograph.printCache();
		  orthograph.optimize(evo);
	      }
	  }
	  
	  // transfer max weight paths to genes + filter + ouput
	  orthograph.outputGenes(optGenes,opt_geneid);
	  offsets.clear();
	  delete geneRange;
	}
    }

    GeneMSA::closeOutputFiles();

    closeOutputFiles(initGenes);
    closeOutputFiles(baseGenes);
    closeOutputFiles(optGenes);

}
