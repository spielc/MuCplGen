#pragma once
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <typeinfo>
#include <functional>
#include <any>
#include "FileLoader.h"
#include "EasyScanner.h"
#include "DebugTool/Highlight.h"
#include "Parser.h"
#include "SealedValue.h"
#include "CustomCodeMacro.h"
#include "CFG.h"

namespace MuCplGen
{
	using namespace MuCplGen::Debug;
	template<typename Parser>
	class SyntaxDirected
	{
	public:
		using Token = typename Parser::Token;
		using TokenSet = std::vector<Token>;
		using Sym = typename Parser::SymbolType;
		using Term = Terminator<Token, Sym>;
	private:
		// A -> ��
		using Production = std::vector<Sym>;
		// A -> ��0|��1|...
		using Productions = std::vector<Production>;
		// A -> ��0|��1|...
		// B -> ��0|��2|...
		// ...
		using ProductionTable = std::vector<Productions>;

	
		Sym start = 0;
		Sym epsilon = 0;
		Sym end = 0;
		Parser my_parser;

		std::unordered_map<std::string, Sym> name_to_sym;
		std::vector<std::string> sym_to_name;


		ProductionTable production_table;

	protected:
		using SemanticAction = std::function<std::any* (std::vector<std::any*>, size_t, TokenSet&)>;
	private:
		std::ostream& log;
		std::vector<Term*> terminator_rules;
		std::vector<ParseRule*> parse_rules;
		std::vector<std::vector<ParseRule*>> quick_parse_rule_table;
		//std::vector<Sym> expects, size_t token_iter
		std::function<void(std::vector<Sym>, size_t)> error_action;

		__declspec(noinline)
		Sym	TokenToTerminator(const Token& token)
		{
			Sym sym = -1;
			size_t priority = -1;
			for (auto term : terminator_rules)
			{
				bool success = term->translation(token);
				if (success && term->priority == 0)
				{
					sym = term->sym;
					break;
				}
				else if (success)
				{
					if (term->priority < priority) priority = term->priority;
					sym = term->sym;
				}
			}
			if (token.IsEndToken()) return end;
			if (sym == (Sym)-1)
				throw(std::logic_error("TokenToTerminator Failed"));
			else return sym;
		}
		__declspec(noinline)
		std::any* SemanticActionDispatcher(std::vector<std::any*> input, size_t nonterm, size_t pro_index, size_t token_iter)
		{
			auto rule = quick_parse_rule_table[nonterm][pro_index];
			this->token_iter = token_iter;
			auto error_pos = NextSemanticError(input);
			if (error_pos != -1)
			{
				if (debug_option & DebugOption::ShowReductionProcess)
				{
					log << rule->expression << " Action{" << rule->action_name << "} => Semantic Error Occurs" << std::endl;
				}
				if (rule->semantic_error) return rule->semantic_error(input);
				else return input[error_pos];
			}
			else
			{
				if (debug_option & DebugOption::ShowReductionProcess)
				{
					log << rule->expression << " Action{" << rule->action_name << "}" << std::endl;
				}
				if (rule->semantic_action == nullptr) return nullptr;
				else return rule->semantic_action(input);
			}
		}
		__declspec(noinline)
		void ErrorReductionActionDispatcher(std::vector<Sym> expects, size_t token_iter)
		{
			if (error_action == nullptr)
			{
				if (debug_option & DebugOption::SyntaxError)
				{
					if (input_text)
					{
						std::stringstream ss;
						ss << "Expected Symbol:";
						for (const auto& item : expects)
							ss << " \'" << sym_to_name[item] << "\' ";
						Highlight(*(input_text), *(token_set), token_iter, ss.str(), log);
					}
					if ((*token_set)[token_iter].IsEndToken())
					{
						if (token_iter > 0) log << "Missing Token After:\n" << (*token_set)[token_iter - 1] << std::endl;
						else log << "No Symbol At All!" << std::endl;
					}
					else log << "Error Token:\n" << (*token_set)[token_iter] << std::endl;
				}
			}
			else error_action(std::move(expects), token_iter);
		}
		std::string ScopedName(const std::string& scope, const std::string& name)
		{
			if (scope.empty()) return name;
			else if (name.find(".") == -1) return scope + "." + name;
			else return name;
		}

		void CheckAndAddSymbol(const std::string& name)
		{
			if (name_to_sym.find(name) == name_to_sym.end())
			{
				name_to_sym.insert({ name, sym_to_name.size() });
				sym_to_name.push_back(name);
			}
		}

		__declspec(noinline)
		void SetupSymbols()
		{
			//start = 0 nonterm epsilon term end
			//terminator
			for (auto& rule : parse_rules)
			{
				std::stringstream ss(rule->expression);
				std::string head;
				ss >> head;
				head = ScopedName(rule->scope, head);
				CheckAndAddSymbol(head);
			}

			production_table.resize(sym_to_name.size());
			quick_parse_rule_table.resize(sym_to_name.size());
			epsilon = sym_to_name.size();
			name_to_sym.insert({ "epsilon",sym_to_name.size() });
			sym_to_name.push_back("epsilon");


			// ruled terminator
			for (auto rule : terminator_rules)
			{
				std::string name = ScopedName(rule->scope, rule->name);
				rule->sym = sym_to_name.size();
				CheckAndAddSymbol(name);
			}

			//wild terminator
			for (auto rule : parse_rules)
			{
				std::stringstream ss(rule->expression);
				std::string head;
				ss >> head;
				Production production;

				std::string axis;
				ss >> axis;
				if (axis != "->") throw("Syntax Error!");
				std::string body;
				while (true)
				{
					body.clear();
					ss >> body;
					if (body.empty()) break;
					auto name = ScopedName(rule->scope, body);
					if (name_to_sym.find(name) == name_to_sym.end())//not a defined nonterm
					{
						if (name_to_sym.find(body) == name_to_sym.end()) //new term
						{
							auto sym = sym_to_name.size();
							name_to_sym.insert({ body, sym });
							sym_to_name.push_back(body);
							production.push_back(sym);

							Term* t = new Term;
							t->name = body;
							t->translation = [t](const Token& token)
							{
								if (t->name == token.name) return true;
								else return false;
							};
							AddTerminator(t);
							t->sym = sym;
						}
						else production.push_back(name_to_sym[body]);
					}
					else production.push_back(name_to_sym[name]);
				}
				head = ScopedName(rule->scope, head);
				production_table[name_to_sym[head]].push_back(std::move(production));
				quick_parse_rule_table[name_to_sym[head]].push_back(rule);
			}
			end = sym_to_name.size();
			sym_to_name.push_back("$");
		}

		size_t token_iter = 0;
	public:
		SyntaxDirected(const std::string& path, std::ostream& log = std::cout) :log(log) {}

		bool Parse(std::vector<LineContent>& input_text, TokenSet& token_set)
		{
			SetInput(input_text);
			return Parse(token_set);
		}

		bool Parse(TokenSet& token_set)
		{
			this->token_set = &token_set;
			return my_parser.Parse(token_set,
				[this](const Token& token) {return TokenToTerminator(token); },
				[this](std::vector<std::any*> input, size_t nonterm, size_t pro_index, size_t token_iter)
				{
					return SemanticActionDispatcher(input, nonterm, pro_index, token_iter);
				},
				[this](std::vector<Sym> expects, size_t token_iter)
				{
					ErrorReductionActionDispatcher(expects, token_iter);
				}, log);
		}

		void SetInput(std::vector<LineContent>& input_text)
		{
			this->input_text = &input_text;
		}

		~SyntaxDirected()
		{
			for (auto rule : parse_rules) delete rule;
			for (auto rule : terminator_rules) delete rule;
		}
		TokenSet* token_set;
#pragma region For Custom Code 
	protected:
		std::vector<LineContent>* input_text = nullptr;
		
		void AddTerminator(Term* terminator)
		{
			terminator_rules.push_back(terminator);
		};

		void AddParseRule(ParseRule* parse_rule)
		{
			parse_rules.push_back(parse_rule);
		};

		const std::string& GetSymbolName(Sym sym)
		{
			return sym_to_name[sym];
		}

		void AddParseErrorAction(std::function<void(std::vector<Sym> expects, size_t token_iter)> action)
		{
			error_action = action;
		}

		bool HasSemanticError(std::vector<std::any*> input)
		{
			return NextSemanticError(input) >= 0;
		}

		__declspec(noinline)
		int NextSemanticError(std::vector<std::any*> input, int last = -1)
		{
			auto first = last + 1;
			auto size = input.size();
			for (int i = first; i < size; ++i)
			{
				if (input[i] == nullptr) continue;
				if (auto p = std::any_cast<ParserErrorCode>(input[i]))
					if (*p == ParserErrorCode::SemanticError) return i;
			}
			return -1;
		}

		void Initialize()
		{
			//CompleteSemanticActionTable();
			SetupSymbols();
			my_parser.SetUp(production_table, end - 1, end, epsilon, start);
			my_parser.debug_option = debug_option;
		}
		
		template<class T>
		T& Get(std::any* a) { return std::any_cast<T&>(*a); }

		const TokenSet& GetTokenSet() { return *token_set; }

		size_t TokenIter() { return token_iter; }

		Token& CurrentToken() { return (*token_set)[token_iter]; }

		const std::vector<LineContent>& InputText() { return *input_text; }

		size_t debug_option = 0;
#pragma endregion
	};

	class TestCompiler :public SyntaxDirected<SLRParser<size_t>>
	{
	public:
		TestCompiler(std::string cfg_path) :SyntaxDirected(cfg_path)
		{

		}
	};	
}